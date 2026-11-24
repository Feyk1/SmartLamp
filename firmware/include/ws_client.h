#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "modbus_client.h"

int ws_client_init(const char *url, const char *token);
void ws_client_deinit(void);
bool ws_client_is_connected(void);

/**
 * @brief Sends the real state to the backend (state on-change).
 *
 * It can be called from another task; sending is serialized.
 */
int ws_client_send_state(const modbus_light_state_t *state);

#endif /* WS_CLIENT_H */
