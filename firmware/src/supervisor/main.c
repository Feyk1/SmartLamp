/**
 * @file main.c
 * @brief Supervisor entry point (simulated PMIC).
 *
 * Resolves the bootloader path (BOOTLOADER_BIN, argv[1] or /proc/self/exe)
 * and delegates to supervisor_run().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "supervisor.h"

#define SUPERVISOR_PATH_MAX 4096

/**
 * @brief Resolves the bootloader path from /proc/self/exe.
 *
 * @param out Output buffer.
 * @param out_size Buffer size.
 */
static void resolve_bootloader_path(char *out, size_t out_size)
{
    ssize_t n;
    char *slash;

    if ((out == NULL) || (out_size == 0U)) {
        return;
    }

    n = readlink("/proc/self/exe", out, out_size - 1U);
    if (n <= 0) {
        (void)snprintf(out, out_size, "bootloader");
        return;
    }
    out[n] = '\0';

    slash = strrchr(out, '/');
    if (slash != NULL) {
        (void)snprintf(slash + 1, out_size - (size_t)(slash + 1 - out),
                       "bootloader");
    }
}

int main(int argc, char **argv)
{
    const char *path = getenv("BOOTLOADER_BIN");
    char resolved[SUPERVISOR_PATH_MAX];

    if ((path == NULL) && (argc > 1)) {
        path = argv[1];
    }
    if (path == NULL) {
        resolve_bootloader_path(resolved, sizeof(resolved));
        path = resolved;
    }

    return (supervisor_run(path) == 0) ? 0 : 1;
}
