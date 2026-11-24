/**
 * @file test_rest_client.c
 * @brief Tests for the minimal HTTP client against a local server.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "unity.h"

#include "log.h"
#include "rest_client.h"

/* Logger stub: the test does not link the HAL (it depends on FreeRTOS). */
void log_write(log_level_t level, const char *file, int line, const char *fmt,
               ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)fmt;
}

typedef struct {
    int listen_fd;
    unsigned short port;
    const char *response;
    char request[2048];
    size_t request_len;
    int accepted;
    pthread_t thread;
} fake_server_t;

/**
 * @brief Tells whether the request has already arrived complete (headers +
 *        Content-Length).
 */
static int request_complete(const char *request)
{
    const char *headers_end = strstr(request, "\r\n\r\n");
    const char *length_header;
    long content_length = 0;

    if (headers_end == NULL) {
        return 0;
    }
    length_header = strstr(request, "Content-Length: ");
    if (length_header != NULL) {
        content_length = strtol(length_header + 16, NULL, 10);
    }
    return (long)(strlen(request) - (size_t)(headers_end + 4 - request)) >=
           content_length;
}

static void *server_main(void *arg)
{
    fake_server_t *s = (fake_server_t *)arg;
    int fd;

    fd = accept(s->listen_fd, NULL, NULL);
    if (fd < 0) {
        return NULL;
    }
    s->accepted = 1;

    while (s->request_len < (sizeof(s->request) - 1U)) {
        ssize_t received = recv(fd, s->request + s->request_len,
                                sizeof(s->request) - 1U - s->request_len, 0);
        if (received <= 0) {
            break;
        }
        s->request_len += (size_t)received;
        s->request[s->request_len] = '\0';
        if (request_complete(s->request) != 0) {
            break;
        }
    }

    (void)send(fd, s->response, strlen(s->response), 0);
    (void)close(fd);
    return NULL;
}

static void server_start(fake_server_t *s, const char *response)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    (void)memset(s, 0, sizeof(*s));
    s->response = response;
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_GREATER_OR_EQUAL(0, s->listen_fd);

    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    TEST_ASSERT_EQUAL_INT(
        0, bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)));
    TEST_ASSERT_EQUAL_INT(0, listen(s->listen_fd, 1));
    TEST_ASSERT_EQUAL_INT(
        0, getsockname(s->listen_fd, (struct sockaddr *)&addr, &addr_len));
    s->port = ntohs(addr.sin_port);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&s->thread, NULL, server_main, s));
}

static void server_stop(fake_server_t *s)
{
    (void)pthread_join(s->thread, NULL);
    (void)close(s->listen_fd);
}

static void client_init(fake_server_t *s)
{
    char url[64];

    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u",
                   (unsigned)s->port);
    TEST_ASSERT_EQUAL_INT(0, rest_client_init(url, "dev-token-local"));
}

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_post_sends_json_and_token(void)
{
    fake_server_t server;
    int status;

    server_start(&server,
                 "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    client_init(&server);

    status = rest_client_post("/api/v1/telemetry/health", "{\"uptime_s\":1}");

    server_stop(&server);
    rest_client_deinit();

    TEST_ASSERT_EQUAL_INT(204, status);
    TEST_ASSERT_NOT_NULL(strstr(
        server.request,
        "POST /api/v1/telemetry/health?token=dev-token-local HTTP/1.1\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(server.request, "Content-Type: application/json"));
    TEST_ASSERT_NOT_NULL(strstr(server.request, "Content-Length: 14"));
    TEST_ASSERT_NOT_NULL(strstr(server.request, "{\"uptime_s\":1}"));
}

static void test_get_returns_body(void)
{
    fake_server_t server;
    char body[256];
    int status;

    server_start(&server,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: 15\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "{\"commands\":[]}");
    client_init(&server);

    status = rest_client_get("/api/v1/commands/pending", body, sizeof(body));

    server_stop(&server);
    rest_client_deinit();

    TEST_ASSERT_EQUAL_INT(200, status);
    TEST_ASSERT_EQUAL_STRING("{\"commands\":[]}", body);
    TEST_ASSERT_NOT_NULL(
        strstr(server.request, "GET /api/v1/commands/pending?token="));
}

typedef struct {
    uint8_t data[256];
    size_t len;
} sink_t;

static int collect_sink(void *ctx, const uint8_t *data, size_t len)
{
    sink_t *sink = (sink_t *)ctx;

    if ((sink->len + len) > sizeof(sink->data)) {
        return -1;
    }
    (void)memcpy(sink->data + sink->len, data, len);
    sink->len += len;
    return 0;
}

static void test_get_stream_collects_body(void)
{
    fake_server_t server;
    sink_t sink;
    size_t total = 0U;
    int status;

    (void)memset(&sink, 0, sizeof(sink));
    server_start(&server,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/octet-stream\r\n"
                 "Content-Length: 5\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "HELLO");
    client_init(&server);

    status = rest_client_get_stream("/api/v1/ota/1.1.0/download", collect_sink,
                                    &sink, &total);

    server_stop(&server);
    rest_client_deinit();

    TEST_ASSERT_EQUAL_INT(200, status);
    TEST_ASSERT_EQUAL_UINT32(5U, total);
    TEST_ASSERT_EQUAL_UINT32(5U, sink.len);
    TEST_ASSERT_EQUAL_MEMORY("HELLO", sink.data, 5U);
    TEST_ASSERT_NOT_NULL(strstr(
        server.request,
        "GET /api/v1/ota/1.1.0/download?token=dev-token-local HTTP/1.1"));
}

static void test_get_stream_skips_error_body(void)
{
    fake_server_t server;
    sink_t sink;
    size_t total = 99U;
    int status;

    (void)memset(&sink, 0, sizeof(sink));
    server_start(&server,
                 "HTTP/1.1 404 Not Found\r\n"
                 "Content-Length: 4\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "nope");
    client_init(&server);

    status = rest_client_get_stream("/api/v1/ota/9.9.9/download", collect_sink,
                                    &sink, &total);

    server_stop(&server);
    rest_client_deinit();

    TEST_ASSERT_EQUAL_INT(404, status);
    TEST_ASSERT_EQUAL_UINT32(0U, total);
    TEST_ASSERT_EQUAL_UINT32(0U, sink.len);
}

static void test_invalid_status_line(void)
{
    fake_server_t server;
    int status;

    server_start(&server, "GARBAGE\r\n\r\n");
    client_init(&server);

    status = rest_client_post("/x", "{}");

    server_stop(&server);
    rest_client_deinit();

    TEST_ASSERT_EQUAL_INT(-1, status);
}

static void test_no_server_returns_zero(void)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    char url[64];
    int fd;
    int status;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    TEST_ASSERT_EQUAL_INT(0, bind(fd, (struct sockaddr *)&addr, sizeof(addr)));
    TEST_ASSERT_EQUAL_INT(0, getsockname(fd, (struct sockaddr *)&addr, &addr_len));
    (void)close(fd);

    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u",
                   (unsigned)ntohs(addr.sin_port));
    TEST_ASSERT_EQUAL_INT(0, rest_client_init(url, "dev-token-local"));

    status = rest_client_post("/x", "{}");
    rest_client_deinit();

    TEST_ASSERT_EQUAL_INT(0, status);
}

int main(void)
{
    (void)signal(SIGPIPE, SIG_IGN);
    UNITY_BEGIN();
    RUN_TEST(test_post_sends_json_and_token);
    RUN_TEST(test_get_returns_body);
    RUN_TEST(test_get_stream_collects_body);
    RUN_TEST(test_get_stream_skips_error_body);
    RUN_TEST(test_invalid_status_line);
    RUN_TEST(test_no_server_returns_zero);
    return UNITY_END();
}
