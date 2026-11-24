/**
 * @file test_net_transport.c
 * @brief Regression: net_transport_recv must survive EINTR.
 *
 * The FreeRTOS POSIX port delivers the tick with SIGALRM every 1 ms and
 * `sa_flags = 0` (no SA_RESTART), so poll()/recv() return EINTR repeatedly.
 * This test reproduces that pattern: it installs a 1 ms signal and checks
 * that reception waits until the data arrives.
 */

#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "unity.h"

#include "net_transport.h"

static volatile sig_atomic_t s_ticks = 0;

/** @brief Tick signal handler (counts only). */
static void on_tick(int signo)
{
    (void)signo;
    s_ticks++;
}

/** @brief Sends one byte 20 ms after the thread starts. */
static void *delayed_send(void *arg)
{
    int fd = *(int *)arg;
    struct timespec delay;

    delay.tv_sec = 0;
    delay.tv_nsec = 20L * 1000L * 1000L;
    (void)nanosleep(&delay, NULL);
    (void)send(fd, "A", 1U, 0);
    return NULL;
}

void setUp(void)
{
    s_ticks = 0;
}

void tearDown(void)
{
}

static void test_recv_survives_tick_signals(void)
{
    int fds[2] = {-1, -1};
    net_transport_t transport;
    struct sigaction sa;
    struct itimerval timer;
    pthread_t thread;
    uint8_t byte = 0U;
    int rc;

    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    transport.fd = fds[1];

    (void)memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_tick;
    sa.sa_flags = 0; /* same as the port: no SA_RESTART */
    (void)sigfillset(&sa.sa_mask);
    TEST_ASSERT_EQUAL_INT(0, sigaction(SIGALRM, &sa, NULL));

    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 1000; /* 1 ms */
    timer.it_interval = timer.it_value;
    TEST_ASSERT_EQUAL_INT(0, setitimer(ITIMER_REAL, &timer, NULL));

    TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, delayed_send,
                                            &fds[0]));
    rc = net_transport_recv(&transport, &byte, 1U, 500U);

    /* Stop the timer before any assert so the rest is unaffected. */
    (void)memset(&timer, 0, sizeof(timer));
    (void)setitimer(ITIMER_REAL, &timer, NULL);
    (void)pthread_join(thread, NULL);

    (void)net_transport_close(&transport);
    (void)close(fds[0]);

    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_EQUAL_UINT8('A', byte);
    TEST_ASSERT_GREATER_THAN(0, (int)s_ticks);
}

static void test_recv_returns_zero_on_timeout(void)
{
    int fds[2] = {-1, -1};
    net_transport_t transport;
    uint8_t byte = 0U;
    int rc;

    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    transport.fd = fds[1];

    rc = net_transport_recv(&transport, &byte, 1U, 50U);

    (void)net_transport_close(&transport);
    (void)close(fds[0]);

    TEST_ASSERT_EQUAL_INT(0, rc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_recv_survives_tick_signals);
    RUN_TEST(test_recv_returns_zero_on_timeout);
    return UNITY_END();
}
