#ifndef MODBUS_SERVER_H
#define MODBUS_SERVER_H

#include <stdint.h>

/**
 * @brief Starts the LED driver's Modbus TCP server.
 *
 * Spawns the animation (20 Hz) and watchdog threads, and serves Modbus TCP
 * clients until SIGTERM/SIGINT is received.
 *
 * @param port TCP port (e.g. 5020).
 * @return 0 on clean exit; -1 on error.
 */
int modbus_server_run(uint16_t port);

#endif /* MODBUS_SERVER_H */
