#ifndef LOG_H
#define LOG_H

#include <stddef.h>

/* --- Log levels ---------------------------------------------------------- */
typedef enum {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_TRACE
} log_level_t;

/* --- Configuration (overridable with -D) -------------------------------- */
#ifndef LOG_LEVEL_MIN
#define LOG_LEVEL_MIN LOG_LEVEL_TRACE   /* highest compiled level; runtime filters */
#endif
#ifndef LOG_LEVEL_DEFAULT
#define LOG_LEVEL_DEFAULT LOG_LEVEL_INFO
#endif
#ifndef LOG_LINE_MAX
#define LOG_LINE_MAX 192
#endif

#define LOG_LOCK_NONE     0
#define LOG_LOCK_CRITICAL 1
#define LOG_LOCK_MUTEX    2
#ifndef LOG_LOCK_STRATEGY
#define LOG_LOCK_STRATEGY LOG_LOCK_MUTEX
#endif

#ifndef LOG_WITH_LOCATION
#define LOG_WITH_LOCATION 0
#endif
#ifndef LOG_AUTO_NEWLINE
#define LOG_AUTO_NEWLINE 1
#endif
#ifndef LOG_USE_ENV_LEVEL
#if defined(__unix__) || defined(__APPLE__)
#define LOG_USE_ENV_LEVEL 1
#else
#define LOG_USE_ENV_LEVEL 0
#endif
#endif

/* --- API ----------------------------------------------------------------- */
typedef void (*log_sink_fn)(const char *buf, size_t len);

void log_init(void);
void log_set_level(log_level_t level);
log_level_t log_get_level(void);
void log_set_sink(log_sink_fn sink);

void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/* --- printf-style macros ------------------------------------------------ */
#if LOG_WITH_LOCATION
#define LOG_LOC_ARGS __FILE__, __LINE__
#else
#define LOG_LOC_ARGS ((const char *)0), 0
#endif

#define LOG_EMIT(level, ...) \
    do { \
        if ((level) <= (LOG_LEVEL_MIN)) { \
            log_write((level), LOG_LOC_ARGS, __VA_ARGS__); \
        } \
    } while (0)

#define LOG_ERROR(...) LOG_EMIT(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_WARN(...)  LOG_EMIT(LOG_LEVEL_WARN,  __VA_ARGS__)
#define LOG_INFO(...)  LOG_EMIT(LOG_LEVEL_INFO,  __VA_ARGS__)
#define LOG_DEBUG(...) LOG_EMIT(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...) LOG_EMIT(LOG_LEVEL_TRACE, __VA_ARGS__)

#endif /* LOG_H */
