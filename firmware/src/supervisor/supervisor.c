/**
 * @file supervisor.c
 * @brief Supervisor (simulated PMIC): device lifecycle.
 *
 * Models the PMIC of ADR-004. The supervisor is the container's main process
 * and stays alive across power-offs so it can receive a `POWER_ON`. It
 * exposes a line-based TCP control channel (ADR-004, amended):
 *
 *   STATUS | POWER_ON | POWER_OFF | RESET | ENTER_BOOTLOADER | FACTORY_RESET
 *
 * The state machine lives in `supervisor_core.c` (testable); the effects
 * (fork/exec, signals, flash erase, poll) are executed here.
 *
 * Critical code: review together with `supervisor_core.c` and the bootloader
 * recovery mode.
 */

#include "supervisor.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "supervisor_core.h"

#define SUPERVISOR_GRACE_MS      3000U
#define SUPERVISOR_POLL_MS       100U
#define SUPERVISOR_DEFAULT_PORT  5022U
#define SUPERVISOR_LINE_MAX      96U
#define SUPERVISOR_PATH_MAX      4096U

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

extern char **environ;

static volatile sig_atomic_t s_stop = 0;
static pid_t s_child_pid = 0;
static char s_boot_path[SUPERVISOR_PATH_MAX];
static char s_flash_path[SUPERVISOR_PATH_MAX];
static const char *s_token = NULL;

/**
 * @brief Async-signal-safe handler for SIGTERM/SIGINT.
 */
static void supervisor_signal_handler(int signo)
{
    int saved_errno = errno;

    (void)signo;
    s_stop = 1;
    errno = saved_errno;
}

/**
 * @brief Supervisor log (main context only).
 */
static void supervisor_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)vfprintf(stdout, fmt, ap);
    va_end(ap);
    (void)fflush(stdout);
}

const char *supervisor_state_name(supervisor_state_t state)
{
    switch (state) {
    case SUPERVISOR_POWERED_OFF:     return "POWERED_OFF";
    case SUPERVISOR_BOOTING:         return "BOOTING";
    case SUPERVISOR_APP_RUNNING:     return "APP_RUNNING";
    case SUPERVISOR_BOOTLOADER_MODE: return "BOOTLOADER_MODE";
    case SUPERVISOR_FAULT:           return "FAULT";
    default:                         return "UNKNOWN";
    }
}

static uint32_t now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(((uint64_t)ts.tv_sec * 1000U) +
                      ((uint64_t)ts.tv_nsec / 1000000U));
}

static void sleep_ms(uint32_t ms)
{
    struct timespec delay;

    delay.tv_sec = (time_t)(ms / 1000U);
    delay.tv_nsec = (long)((ms % 1000U) * 1000000L);
    (void)nanosleep(&delay, NULL);
}

/**
 * @brief Logs the state transition if it changed.
 */
static void log_state(sp_state_t *last, sp_state_t current)
{
    if (*last != current) {
        supervisor_log("[supervisor] state: %s\n",
                       sp_state_name(current));
        *last = current;
    }
}

/**
 * @brief Creates the control socket (0.0.0.0:port).
 */
static int create_listener(uint16_t port)
{
    int fd;
    int one = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if ((bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) ||
        (listen(fd, 4) != 0)) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/**
 * @brief Reads a line (up to `\n`) with a socket timeout.
 *
 * @return 0 on success; -1 if the client closes or times out.
 */
static int read_line(int fd, char *buffer, size_t size)
{
    size_t used = 0U;

    while ((used + 1U) < size) {
        ssize_t n = recv(fd, buffer + used, 1U, 0);

        if (n <= 0) {
            return -1;
        }
        if (buffer[used] == '\n') {
            break;
        }
        used++;
    }
    while ((used > 0U) && ((buffer[used - 1U] == '\r') ||
                           (buffer[used - 1U] == ' '))) {
        used--;
    }
    buffer[used] = '\0';
    return 0;
}

/**
 * @brief Replies with the current state as one-line JSON.
 */
static void send_state(int fd, const sp_core_t *core)
{
    char response[192];
    int n;

    n = snprintf(response, sizeof(response),
                 "{\"state\":\"%s\",\"pid\":%d,\"forced\":%s,"
                 "\"last_exit\":%d}\n",
                 sp_state_name(core->state), (int)s_child_pid,
                 core->forced ? "true" : "false",
                 core->has_exit_status ? core->last_exit_status : -1);
    if (n > 0) {
        (void)send(fd, response, (size_t)n, MSG_NOSIGNAL);
    }
}

/**
 * @brief Starts the bootloader in a child process.
 *
 * @return true if fork/exec was launched.
 */
static bool spawn_child(bool forced)
{
    sigset_t block;
    sigset_t old;
    sigset_t empty;
    pid_t pid;

    (void)sigemptyset(&block);
    (void)sigaddset(&block, SIGTERM);
    (void)sigaddset(&block, SIGINT);
    (void)sigprocmask(SIG_BLOCK, &block, &old);

    pid = fork();
    if (pid < 0) {
        (void)sigprocmask(SIG_SETMASK, &old, NULL);
        return false;
    }
    if (pid == 0) {
        struct sigaction dfl;

        (void)memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        (void)sigaction(SIGTERM, &dfl, NULL);
        (void)sigaction(SIGINT, &dfl, NULL);
        (void)sigemptyset(&empty);
        (void)sigprocmask(SIG_SETMASK, &empty, NULL);
        (void)setenv("FORCE_BOOTLOADER", forced ? "1" : "0", 1);

        {
            char *const argv[] = { s_boot_path, NULL };

            (void)execve(s_boot_path, argv, environ);
        }
        _exit(127);
    }

    (void)sigprocmask(SIG_SETMASK, &old, NULL);
    s_child_pid = pid;
    return true;
}

/**
 * @brief Stops the child with a 3 s grace period and SIGKILL as fallback.
 */
static void stop_child(const char *reason)
{
    uint32_t deadline;
    int status = 0;

    if (s_child_pid <= 0) {
        return;
    }

    supervisor_log("[supervisor] %s: SIGTERM to child (pid=%d)\n", reason,
                   (int)s_child_pid);
    (void)kill(s_child_pid, SIGTERM);

    deadline = now_ms() + SUPERVISOR_GRACE_MS;
    for (;;) {
        pid_t r = waitpid(s_child_pid, &status, WNOHANG);

        if ((r == s_child_pid) || ((r < 0) && (errno == ECHILD))) {
            break;
        }
        if ((int32_t)(now_ms() - deadline) >= 0) {
            supervisor_log("[supervisor] grace period expired: SIGKILL\n");
            (void)kill(s_child_pid, SIGKILL);
            (void)waitpid(s_child_pid, &status, 0);
            break;
        }
        sleep_ms(SUPERVISOR_POLL_MS);
    }
    s_child_pid = 0;
}

/**
 * @brief Starts the bootloader while applying the state transition.
 */
static void start_child(sp_core_t *core, bool forced)
{
    if (!sp_on_power_on(core, forced, now_ms())) {
        return;
    }
    if (!spawn_child(forced)) {
        supervisor_log("[supervisor] failed to launch %s\n", s_boot_path);
        sp_set_powered_off(core);
        return;
    }
    if (forced) {
        sp_set_bootloader_mode(core);
    }
}

/**
 * @brief Executes a control command.
 */
static void execute_command(sp_core_t *core, sp_cmd_t cmd)
{
    switch (cmd) {
    case SP_CMD_STATUS:
        break;

    case SP_CMD_POWER_ON:
        if (core->state == SP_BOOTLOADER_MODE) {
            stop_child("exit bootloader");
            sp_set_powered_off(core);
        }
        start_child(core, false);
        break;

    case SP_CMD_POWER_OFF:
        stop_child("power off");
        sp_set_powered_off(core);
        break;

    case SP_CMD_RESET:
        stop_child("reset");
        sp_set_powered_off(core);
        start_child(core, false);
        break;

    case SP_CMD_ENTER_BOOTLOADER:
        stop_child("enter bootloader");
        sp_set_powered_off(core);
        start_child(core, true);
        break;

    case SP_CMD_FACTORY_RESET:
        stop_child("factory reset");
        if (unlink(s_flash_path) == 0) {
            supervisor_log("[supervisor] factory reset: %s deleted\n",
                           s_flash_path);
        } else {
            supervisor_log("[supervisor] factory reset: failed to delete "
                           "%s (%s)\n", s_flash_path, strerror(errno));
        }
        sp_set_powered_off(core);
        start_child(core, false);
        break;

    default:
        break;
    }
}

/**
 * @brief Serves a control connection (one command per connection).
 */
static void handle_client(int fd, sp_core_t *core, sp_state_t *logged)
{
    struct timeval timeout;
    char line[SUPERVISOR_LINE_MAX];
    char *command = line;

    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (read_line(fd, line, sizeof(line)) != 0) {
        return;
    }

    if (s_token != NULL) {
        char *space = strchr(line, ' ');

        if (space == NULL) {
            static const char unauthorized[] =
                "{\"error\":\"UNAUTHORIZED\"}\n";

            (void)send(fd, unauthorized, sizeof(unauthorized) - 1U,
                       MSG_NOSIGNAL);
            return;
        }
        *space = '\0';
        if (strcmp(line, s_token) != 0) {
            static const char unauthorized[] =
                "{\"error\":\"UNAUTHORIZED\"}\n";

            (void)send(fd, unauthorized, sizeof(unauthorized) - 1U,
                       MSG_NOSIGNAL);
            return;
        }
        command = space + 1;
    }

    {
        sp_cmd_t cmd = sp_parse_command(command);

        if (cmd == SP_CMD_INVALID) {
            static const char invalid[] =
                "{\"error\":\"INVALID_COMMAND\"}\n";

            (void)send(fd, invalid, sizeof(invalid) - 1U, MSG_NOSIGNAL);
            return;
        }
        execute_command(core, cmd);
    }

    log_state(logged, core->state);
    send_state(fd, core);
}

int supervisor_run(const char *boot_path)
{
    struct sigaction sa;
    sp_core_t core;
    sp_state_t logged = (sp_state_t)255;
    const char *env;
    long port = (long)SUPERVISOR_DEFAULT_PORT;
    int listener;

    if ((boot_path == NULL) || (strlen(boot_path) >= sizeof(s_boot_path))) {
        return -1;
    }
    (void)snprintf(s_boot_path, sizeof(s_boot_path), "%s", boot_path);

    env = getenv("FLASH_PATH");
    (void)snprintf(s_flash_path, sizeof(s_flash_path), "%s",
                   (env != NULL) ? env : "flash.bin");
    env = getenv("SUPERVISOR_PORT");
    if (env != NULL) {
        port = strtol(env, NULL, 10);
    }
    if ((port <= 0L) || (port > 65535L)) {
        port = (long)SUPERVISOR_DEFAULT_PORT;
    }
    env = getenv("SUPERVISOR_TOKEN");
    s_token = ((env != NULL) && (env[0] != '\0')) ? env : NULL;

    s_stop = 0;
    s_child_pid = 0;
    sp_init(&core);

    (void)memset(&sa, 0, sizeof(sa));
    sa.sa_handler = supervisor_signal_handler;
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: EINTR is expected on poll/accept */

    if ((sigaction(SIGTERM, &sa, NULL) != 0) ||
        (sigaction(SIGINT, &sa, NULL) != 0)) {
        supervisor_log("[supervisor] failed to install signal handlers\n");
        return -1;
    }
    (void)signal(SIGPIPE, SIG_IGN);

    listener = create_listener((uint16_t)port);
    if (listener < 0) {
        supervisor_log("[supervisor] failed to listen on %ld (%s)\n",
                       port, strerror(errno));
        return -1;
    }

    supervisor_log("[supervisor] control channel on 0.0.0.0:%ld\n", port);
    log_state(&logged, core.state);

    /* Automatic power-on at start-up (like plugging in the device); the
     * panel can power it off and back on later. */
    start_child(&core, false);
    log_state(&logged, core.state);

    while (!s_stop) {
        struct pollfd pfd;

        pfd.fd = listener;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, (int)SUPERVISOR_POLL_MS) > 0) {
            if ((pfd.revents & POLLIN) != 0) {
                int fd = accept(listener, NULL, NULL);

                if (fd >= 0) {
                    handle_client(fd, &core, &logged);
                    (void)close(fd);
                }
            }
        }

        /* Unexpected child crash. */
        if (s_child_pid > 0) {
            int status = 0;
            pid_t r = waitpid(s_child_pid, &status, WNOHANG);

            if (r == s_child_pid) {
                s_child_pid = 0;
                sp_on_child_exit(&core, status);
                if (core.state == SP_FAULT) {
                    supervisor_log("[supervisor] FAULT: child exited "
                                   "(status=0x%x)\n", (unsigned)status);
                }
            }
        }

        (void)sp_on_boot_timeout(&core, now_ms());
        log_state(&logged, core.state);
    }

    stop_child("supervisor shutdown");
    sp_set_powered_off(&core);
    log_state(&logged, core.state);
    (void)close(listener);
    supervisor_log("[supervisor] stopped\n");
    return 0;
}
