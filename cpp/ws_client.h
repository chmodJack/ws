#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ws_connection ws_connection_t;

typedef enum {
    WS_TYPE_TEXT = 1,
    WS_TYPE_BINARY = 2
} ws_type_t;

/* Connects synchronously. timeout_ms=0 uses a 10-second timeout. */
ws_connection_t *ws_open(const char *url, int timeout_ms);

/* Sends exactly one complete text or binary WebSocket message. */
ssize_t ws_write(ws_connection_t *connection, const void *data, size_t length,
                 ws_type_t type);

/* Reads one complete message. timeout_ms=0 waits indefinitely.
 * On EMSGSIZE, message_length reports the required capacity and the message
 * stays queued for the next call. */
ssize_t ws_read(ws_connection_t *connection, void *buffer, size_t capacity,
                size_t *message_length, ws_type_t *type, int timeout_ms);

void ws_close(ws_connection_t *connection);

#ifdef __cplusplus
}
#endif

#endif
