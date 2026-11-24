/**
 * @file main.c
 * @brief Entry point of the LED driver (Modbus TCP server).
 */

#include <stdio.h>
#include <stdlib.h>

#include "modbus_server.h"

int main(void)
{
    const char *env_port = getenv("MODBUS_PORT");
    long port = (env_port != NULL) ? strtol(env_port, NULL, 10) : 5020L;

    if ((port <= 0L) || (port > 65535L)) {
        port = 5020L;
    }

    (void)fprintf(stdout, "[driver] starting (port %ld)\n", port);
    (void)fflush(stdout);

    return (modbus_server_run((uint16_t)port) == 0) ? 0 : 1;
}
