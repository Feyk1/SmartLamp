/**
 * @file log.c
 * @brief Lightweight logging for the firmware.
 *
 * Formatting happens outside the lock; only the write to the sink is
 * serialized. The sink and the locking strategy are configurable so the code
 * can be ported to an MCU.
 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#if LOG_USE_ENV_LEVEL
#include <stdlib.h>
#include <strings.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#define LOG_HAVE_POSIX_SINK 1
#include <errno.h>
#include <unistd.h>
#else
#define LOG_HAVE_POSIX_SINK 0
#endif

#if (LOG_LOCK_STRATEGY == LOG_LOCK_MUTEX)
static SemaphoreHandle_t s_log_mutex = NULL;
#endif

static log_level_t s_log_level = LOG_LEVEL_DEFAULT;
static log_sink_fn s_sink = NULL;

/**
 * @brief Returns the short name of a log level.
 *
 * @param level Log level.
 * @return Constant string with the name.
 */
static const char *log_level_name(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_WARN:  return "WARN ";
    case LOG_LEVEL_INFO:  return "INFO ";
    case LOG_LEVEL_DEBUG: return "DEBUG";
    case LOG_LEVEL_TRACE: return "TRACE";
    default:              return "NONE ";
    }
}

#if LOG_HAVE_POSIX_SINK
/**
 * @brief Default sink: writes the line to stdout (without stdio).
 *
 * @param buf Buffer with the already formatted line.
 * @param len Length in bytes.
 */
static void log_sink_stdout(const char *buf, size_t len)
{
    size_t off = 0U;

    while (off < len) {
        ssize_t written = write(STDOUT_FILENO, buf + off, len - off);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (written == 0) {
            break;
        }
        off += (size_t)written;
    }
}
#endif

/**
 * @brief Takes the logging lock according to LOG_LOCK_STRATEGY.
 */
static void log_lock(void)
{
#if (LOG_LOCK_STRATEGY == LOG_LOCK_MUTEX)
    if (s_log_mutex != NULL) {
        (void)xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    }
#elif (LOG_LOCK_STRATEGY == LOG_LOCK_CRITICAL)
    taskENTER_CRITICAL();
#endif
}

/**
 * @brief Releases the logging lock according to LOG_LOCK_STRATEGY.
 */
static void log_unlock(void)
{
#if (LOG_LOCK_STRATEGY == LOG_LOCK_MUTEX)
    if (s_log_mutex != NULL) {
        (void)xSemaphoreGive(s_log_mutex);
    }
#elif (LOG_LOCK_STRATEGY == LOG_LOCK_CRITICAL)
    taskEXIT_CRITICAL();
#endif
}

void log_init(void)
{
    s_log_level = LOG_LEVEL_DEFAULT;

#if (LOG_LOCK_STRATEGY == LOG_LOCK_MUTEX)
    s_log_mutex = xSemaphoreCreateMutex();
    configASSERT(s_log_mutex != NULL);
#endif

#if LOG_HAVE_POSIX_SINK
    if (s_sink == NULL) {
        s_sink = log_sink_stdout;
    }
#endif

#if LOG_USE_ENV_LEVEL
    {
        const char *env = getenv("LOG_LEVEL");
        if (env != NULL) {
            if (strcasecmp(env, "none") == 0) {
                log_set_level(LOG_LEVEL_NONE);
            } else if (strcasecmp(env, "error") == 0) {
                log_set_level(LOG_LEVEL_ERROR);
            } else if (strcasecmp(env, "warn") == 0) {
                log_set_level(LOG_LEVEL_WARN);
            } else if (strcasecmp(env, "info") == 0) {
                log_set_level(LOG_LEVEL_INFO);
            } else if (strcasecmp(env, "debug") == 0) {
                log_set_level(LOG_LEVEL_DEBUG);
            } else if (strcasecmp(env, "trace") == 0) {
                log_set_level(LOG_LEVEL_TRACE);
            }
        }
    }
#endif
}

void log_set_level(log_level_t level)
{
    if (level < LOG_LEVEL_NONE) {
        level = LOG_LEVEL_NONE;
    }
    if (level > LOG_LEVEL_TRACE) {
        level = LOG_LEVEL_TRACE;
    }
    s_log_level = level;
}

log_level_t log_get_level(void)
{
    return s_log_level;
}

void log_set_sink(log_sink_fn sink)
{
    if (sink != NULL) {
        s_sink = sink;
    } else {
#if LOG_HAVE_POSIX_SINK
        s_sink = log_sink_stdout;
#else
        s_sink = NULL;
#endif
    }
}

void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...)
{
    char buf[LOG_LINE_MAX];
    size_t used;
    int n;
    va_list ap;
    const char *task_name;

    if ((level == LOG_LEVEL_NONE) || (level > s_log_level) || (fmt == NULL)) {
        return;
    }

    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        task_name = pcTaskGetName(NULL);
        if (task_name == NULL) {
            task_name = "main";
        }
    } else {
        task_name = "main";
    }

#if LOG_WITH_LOCATION
    n = snprintf(buf, sizeof(buf), "[%lu ms][%s][%s][%s:%d] ",
                 (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                 log_level_name(level), task_name, file, line);
#else
    (void)file;
    (void)line;
    n = snprintf(buf, sizeof(buf), "[%lu ms][%s][%s] ",
                 (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                 log_level_name(level), task_name);
#endif
    if (n < 0) {
        return;
    }
    used = ((size_t)n < sizeof(buf)) ? (size_t)n : (sizeof(buf) - 1U);

    va_start(ap, fmt);
    n = vsnprintf(buf + used, sizeof(buf) - used, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t avail = sizeof(buf) - used;
        used += ((size_t)n < avail) ? (size_t)n : (avail - 1U);
    }

#if LOG_AUTO_NEWLINE
    if ((used == 0U) || (buf[used - 1U] != '\n')) {
        if (used < (sizeof(buf) - 1U)) {
            buf[used] = '\n';
            used++;
        }
    }
#endif
    buf[used] = '\0';

    log_lock();
    if (s_sink != NULL) {
        s_sink(buf, used);
    }
    log_unlock();
}
