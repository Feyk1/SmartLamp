#ifndef REST_CLIENT_H
#define REST_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Minimal HTTP/1.1 client for the firmware REST telemetry.
 * No TLS yet (the local demo uses http://); the socket transport is the same
 * HAL the WebSocket client uses.
 */

/**
 * @brief Initializes the client with the orchestrator base URL.
 *
 * @param base_url Base URL, e.g. "http://backend:8080" or
 *                 "http://127.0.0.1:8080".
 * @param token    `device_token` appended as `?token=`.
 * @return 0 if the URL is valid; -1 otherwise.
 */
int rest_client_init(const char *base_url, const char *token);

/** @brief Releases the client state. */
void rest_client_deinit(void);

/** @brief Tells whether the client was initialized successfully. */
bool rest_client_is_ready(void);

/**
 * @brief Sends a JSON POST.
 *
 * @param path Absolute path (`/api/v1/telemetry/health`).
 * @param json JSON body (may be NULL for a POST with no body).
 * @return HTTP status (200-299), 0 if there was no response, -1 on error.
 */
int rest_client_post(const char *path, const char *json);

/**
 * @brief Sends a GET and copies the response body.
 *
 * @param path     Absolute path.
 * @param out      Output buffer (NUL-terminated; may be NULL).
 * @param out_size Buffer size.
 * @return HTTP status (200-299), 0 if there was no response, -1 on error.
 */
int rest_client_get(const char *path, char *out, size_t out_size);

/** @brief Incremental receive callback for a response body. */
typedef int (*rest_sink_fn)(void *ctx, const uint8_t *data, size_t len);

/**
 * @brief GET with the body streamed (to download a `.ota`).
 *
 * The callback receives the body as it arrives; a non-zero return aborts the
 * download.
 *
 * @param path  Absolute path.
 * @param sink  Write callback.
 * @param ctx   Callback context.
 * @param total Bytes delivered (may be NULL).
 * @return HTTP status, 0 if there was no response, -1 on error.
 */
int rest_client_get_stream(const char *path, rest_sink_fn sink, void *ctx,
                           size_t *total);

#endif /* REST_CLIENT_H */
