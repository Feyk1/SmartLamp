/**
 * @file modbus_server.c
 * @brief Modbus TCP server for the LED driver (libmodbus).
 *
 * The main thread serves clients; one thread animates at 20 Hz and another
 * monitors the heartbeat. Shared state is protected by the register_map mutex.
 */

#include "modbus_server.h"

#include <modbus.h>
#include <modbus-tcp.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "register_map.h"

static volatile sig_atomic_t s_stop = 0;

/**
 * @brief Async-signal-safe handler for SIGTERM/SIGINT.
 */
static void server_signal_handler(int signo)
{
    (void)signo;
    s_stop = 1;
}

/**
 * @brief Animation thread: map tick at 20 Hz.
 */
static void *animation_thread(void *arg)
{
    const struct timespec delay = {0, 50L * 1000L * 1000L};

    (void)arg;

    while (!s_stop) {
        register_map_tick(50U);
        (void)nanosleep(&delay, NULL);
    }
    return NULL;
}

/**
 * @brief Watchdog thread: checks the heartbeat every second.
 */
static void *watchdog_thread(void *arg)
{
    const struct timespec delay = {1, 0};
    bool was_lost = false;

    (void)arg;

    while (!s_stop) {
        bool lost;

        register_map_watchdog_tick();
        lost = register_map_get_discrete_bit(
                   (uint16_t)DISCRETE_STATUS_HEARTBEAT_LOST) != 0U;

        if (lost && !was_lost) {
            (void)fprintf(stdout, "[driver] heartbeat lost: LED off\n");
            (void)fflush(stdout);
        } else if (!lost && was_lost) {
            (void)fprintf(stdout, "[driver] heartbeat recovered\n");
            (void)fflush(stdout);
        }
        was_lost = lost;

        (void)nanosleep(&delay, NULL);
    }
    return NULL;
}

/**
 * @brief Applies only the registers written by the request to the register_map.
 *
 * The mapping is filled with the current state before modbus_reply; after
 * modbus_reply it holds the writes. Copying the whole mapping would overwrite
 * internal command changes (e.g. CMD_POWER_ON updates TARGET_POWER). That is
 * why only the range written by FC05/06/15/16 is copied.
 */
static void apply_request_to_register_map(const uint8_t *query, int length,
                                          modbus_mapping_t *mapping)
{
    uint8_t function;
    uint16_t addr;
    uint16_t count;
    uint16_t i;

    if ((query == NULL) || (mapping == NULL) || (length < 8)) {
        return;
    }

    function = query[7]; /* 7-byte MBAP; the PDU starts at 7. */

    if (function == 0x05U) {
        if (length < 12) {
            return;
        }
        addr = ((uint16_t)query[8] << 8) | (uint16_t)query[9];
        if (addr < (uint16_t)COIL_COUNT) {
            uint8_t coils[COIL_COUNT];

            register_map_get_coils(coils, (uint16_t)COIL_COUNT);
            coils[addr] = mapping->tab_bits[addr];
            register_map_set_coils(coils, (uint16_t)COIL_COUNT);
            register_map_get_coils(mapping->tab_bits, (uint16_t)COIL_COUNT);
        }
    } else if (function == 0x0FU) {
        if (length < 13) {
            return;
        }
        addr = ((uint16_t)query[8] << 8) | (uint16_t)query[9];
        count = ((uint16_t)query[10] << 8) | (uint16_t)query[11];
        if (addr < (uint16_t)COIL_COUNT) {
            uint8_t coils[COIL_COUNT];

            register_map_get_coils(coils, (uint16_t)COIL_COUNT);
            for (i = 0U; (i < count) && ((addr + i) < (uint16_t)COIL_COUNT);
                 i++) {
                coils[addr + i] = mapping->tab_bits[addr + i];
            }
            register_map_set_coils(coils, (uint16_t)COIL_COUNT);
            register_map_get_coils(mapping->tab_bits, (uint16_t)COIL_COUNT);
        }
    } else if (function == 0x06U) {
        if (length < 12) {
            return;
        }
        addr = ((uint16_t)query[8] << 8) | (uint16_t)query[9];
        if (addr < (uint16_t)HOLDING_COUNT) {
            uint16_t holding[HOLDING_COUNT];

            register_map_get_holding(holding, (uint16_t)HOLDING_COUNT);
            holding[addr] = mapping->tab_registers[addr];
            register_map_set_holding(holding, (uint16_t)HOLDING_COUNT);
        }
    } else if (function == 0x10U) {
        if (length < 13) {
            return;
        }
        addr = ((uint16_t)query[8] << 8) | (uint16_t)query[9];
        count = ((uint16_t)query[10] << 8) | (uint16_t)query[11];
        if (addr < (uint16_t)HOLDING_COUNT) {
            uint16_t holding[HOLDING_COUNT];

            register_map_get_holding(holding, (uint16_t)HOLDING_COUNT);
            for (i = 0U; (i < count) && ((addr + i) < (uint16_t)HOLDING_COUNT);
                 i++) {
                holding[addr + i] = mapping->tab_registers[addr + i];
            }
            register_map_set_holding(holding, (uint16_t)HOLDING_COUNT);
        }
    }
}

/** @brief Context of a Modbus client served by its own thread. */
typedef struct {
    modbus_t *ctx;
    int fd;
} client_ctx_t;

/**
 * @brief Serves a client until it disconnects.
 *
 * Each client has its own libmodbus context and its own mapping; shared state
 * (register_map) is protected by its mutex. This lets the firmware and the
 * backend (Simulation Panel) read and write concurrently.
 */
static void *client_thread(void *arg)
{
    client_ctx_t *client = (client_ctx_t *)arg;
    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
    modbus_mapping_t *mapping;

    mapping = modbus_mapping_new(COIL_COUNT, DISCRETE_COUNT, HOLDING_COUNT,
                                 INPUT_COUNT);
    if (mapping == NULL) {
        modbus_close(client->ctx);
        modbus_free(client->ctx);
        free(client);
        return NULL;
    }

    (void)fprintf(stdout, "[driver] client connected\n");
    (void)fflush(stdout);

    while (!s_stop) {
        int rc = modbus_receive(client->ctx, query);

        if (rc > 0) {
            register_map_get_coils(mapping->tab_bits,
                                   (uint16_t)COIL_COUNT);
            register_map_get_discrete(mapping->tab_input_bits,
                                      (uint16_t)DISCRETE_COUNT);
            register_map_get_holding(mapping->tab_registers,
                                     (uint16_t)HOLDING_COUNT);
            register_map_get_input(mapping->tab_input_registers,
                                   (uint16_t)INPUT_COUNT);

            (void)modbus_reply(client->ctx, query, rc, mapping);

            apply_request_to_register_map(query, rc, mapping);
        } else if (rc == 0) {
            continue;
        } else {
            break;
        }
    }

    modbus_close(client->ctx);
    modbus_free(client->ctx);
    modbus_mapping_free(mapping);
    free(client);

    (void)fprintf(stdout, "[driver] client disconnected\n");
    (void)fflush(stdout);
    return NULL;
}

int modbus_server_run(uint16_t port)
{
    struct sigaction sa;
    modbus_t *ctx = NULL;
    int server_socket = -1;
    pthread_t anim_thread;
    pthread_t wd_thread;
    bool anim_started = false;
    bool wd_started = false;
    int result = -1;

    s_stop = 0;
    register_map_init();

    (void)memset(&sa, 0, sizeof(sa));
    sa.sa_handler = server_signal_handler;
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: EINTR is expected in accept */

    if ((sigaction(SIGTERM, &sa, NULL) != 0) ||
        (sigaction(SIGINT, &sa, NULL) != 0)) {
        (void)fprintf(stderr, "[driver] failed to install signal handlers\n");
        return -1;
    }

    ctx = modbus_new_tcp(NULL, (int)port);
    if (ctx == NULL) {
        (void)fprintf(stderr, "[driver] modbus_new_tcp failed\n");
        goto cleanup;
    }
    (void)modbus_set_slave(ctx, 1);

    /* Backlog of 8: firmware + backend + retries. */
    server_socket = modbus_tcp_listen(ctx, 8);
    if (server_socket == -1) {
        (void)fprintf(stderr, "[driver] failed to listen on port %u\n",
                      (unsigned)port);
        goto cleanup;
    }

    if (pthread_create(&anim_thread, NULL, animation_thread, NULL) != 0) {
        (void)fprintf(stderr, "[driver] failed to create animation thread\n");
        goto cleanup;
    }
    anim_started = true;

    if (pthread_create(&wd_thread, NULL, watchdog_thread, NULL) != 0) {
        (void)fprintf(stderr, "[driver] failed to create watchdog thread\n");
        goto cleanup;
    }
    wd_started = true;

    (void)fprintf(stdout, "[driver] Modbus TCP listening on 0.0.0.0:%u\n",
                  (unsigned)port);
    (void)fflush(stdout);

    while (!s_stop) {
        int fd = accept(server_socket, NULL, NULL);

        if (fd < 0) {
            if (s_stop) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            {
                const struct timespec pause = {0, 10L * 1000L * 1000L};
                (void)nanosleep(&pause, NULL);
            }
            continue;
        }

        {
            modbus_t *client = modbus_new_tcp(NULL, (int)port);
            client_ctx_t *client_ctx;
            pthread_t thread;

            if (client == NULL) {
                (void)close(fd);
                continue;
            }
            (void)modbus_set_slave(client, 1);
            if (modbus_set_socket(client, fd) != 0) {
                modbus_free(client);
                (void)close(fd);
                continue;
            }
            client_ctx = (client_ctx_t *)malloc(sizeof(*client_ctx));
            if (client_ctx == NULL) {
                modbus_close(client);
                modbus_free(client);
                continue;
            }
            client_ctx->ctx = client;
            client_ctx->fd = fd;

            if (pthread_create(&thread, NULL, client_thread, client_ctx) != 0) {
                modbus_close(client);
                modbus_free(client);
                free(client_ctx);
                continue;
            }
            (void)pthread_detach(thread);
        }
    }

    result = 0;

cleanup:
    s_stop = 1;

    if (anim_started) {
        (void)pthread_join(anim_thread, NULL);
    }
    if (wd_started) {
        (void)pthread_join(wd_thread, NULL);
    }
    if (ctx != NULL) {
        modbus_close(ctx); /* also closes the listening socket */
        modbus_free(ctx);
        server_socket = -1;
    }
    if (server_socket != -1) {
        (void)close(server_socket);
    }

    (void)fprintf(stdout, "[driver] stopped\n");
    (void)fflush(stdout);
    return result;
}
