#define _POSIX_C_SOURCE 200809L

#include "ws_client.h"

#include <libwebsockets.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WS_RX_BUFFER_SIZE 4096
#define WS_DEFAULT_CONNECT_TIMEOUT_MS 10000

struct ws_message {
    struct ws_message *next;
    ws_type_t type;
    size_t length;
    unsigned char data[];
};

struct ws_write_request {
    struct ws_write_request *next;
    pthread_cond_t completed_cond;
    unsigned char *buffer;
    size_t length;
    ws_type_t type;
    ssize_t result;
    int error;
    int completed;
};

struct ws_connection {
    struct lws_context *context;
    struct lws *wsi;
    struct lws_protocols protocols[2];
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;

    char *address;
    char *host_header;
    char *path;
    int port;
    int use_tls;
    int thread_started;
    int connected;
    int closed;
    int stopping;
    int error_code;

    struct ws_write_request *write_head;
    struct ws_write_request *write_tail;

    struct ws_message *read_head;
    struct ws_message *read_tail;
    unsigned char *rx_buffer;
    size_t rx_length;
    size_t rx_capacity;
    ws_type_t rx_type;
    int rx_in_progress;
};

static pthread_once_t log_config_once = PTHREAD_ONCE_INIT;

static void configure_lws_logging(void)
{
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
}

static void add_milliseconds(struct timespec *time, int milliseconds)
{
    time->tv_sec += milliseconds / 1000;
    time->tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (time->tv_nsec >= 1000000000L) {
        ++time->tv_sec;
        time->tv_nsec -= 1000000000L;
    }
}

static int is_terminal(const struct ws_connection *connection)
{
    return connection->closed || connection->error_code != 0;
}

static void fail_writes_locked(struct ws_connection *connection, int error)
{
    struct ws_write_request *request = connection->write_head;

    connection->write_head = NULL;
    connection->write_tail = NULL;
    while (request) {
        struct ws_write_request *next = request->next;

        request->result = -1;
        request->error = error;
        request->completed = 1;
        pthread_cond_signal(&request->completed_cond);
        request = next;
    }
}

static void free_messages(struct ws_connection *connection)
{
    struct ws_message *message = connection->read_head;

    while (message) {
        struct ws_message *next = message->next;
        free(message);
        message = next;
    }
    connection->read_head = NULL;
    connection->read_tail = NULL;
}

static int queue_message_locked(struct ws_connection *connection)
{
    struct ws_message *message;

    if (connection->rx_length > SIZE_MAX - sizeof(*message)) {
        return -1;
    }
    message = (struct ws_message *)malloc(sizeof(*message) + connection->rx_length);
    if (!message) {
        return -1;
    }

    message->next = NULL;
    message->type = connection->rx_type;
    message->length = connection->rx_length;
    if (connection->rx_length) {
        memcpy(message->data, connection->rx_buffer, connection->rx_length);
    }

    if (connection->read_tail) {
        connection->read_tail->next = message;
    } else {
        connection->read_head = message;
    }
    connection->read_tail = message;
    connection->rx_length = 0;
    connection->rx_in_progress = 0;
    pthread_cond_broadcast(&connection->changed);
    return 0;
}

static int receive_data(struct ws_connection *connection, struct lws *wsi,
                        const void *data, size_t length)
{
    size_t required;
    size_t capacity;
    unsigned char *buffer;
    int complete;

    pthread_mutex_lock(&connection->mutex);
    if (!connection->rx_in_progress) {
        connection->rx_type = lws_frame_is_binary(wsi) ? WS_TYPE_BINARY : WS_TYPE_TEXT;
        connection->rx_in_progress = 1;
    }

    if (length > SIZE_MAX - connection->rx_length) {
        connection->error_code = EMSGSIZE;
        pthread_cond_broadcast(&connection->changed);
        pthread_mutex_unlock(&connection->mutex);
        return -1;
    }

    required = connection->rx_length + length;
    if (required > connection->rx_capacity) {
        capacity = connection->rx_capacity ? connection->rx_capacity : WS_RX_BUFFER_SIZE;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        buffer = (unsigned char *)realloc(connection->rx_buffer, capacity);
        if (!buffer) {
            connection->error_code = ENOMEM;
            pthread_cond_broadcast(&connection->changed);
            pthread_mutex_unlock(&connection->mutex);
            return -1;
        }
        connection->rx_buffer = buffer;
        connection->rx_capacity = capacity;
    }

    if (length) {
        memcpy(connection->rx_buffer + connection->rx_length, data, length);
    }
    connection->rx_length = required;
    complete = lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0;
    if (complete && queue_message_locked(connection) != 0) {
        connection->error_code = ENOMEM;
        pthread_cond_broadcast(&connection->changed);
        pthread_mutex_unlock(&connection->mutex);
        return -1;
    }

    pthread_mutex_unlock(&connection->mutex);
    return 0;
}

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t length)
{
    struct ws_connection *connection;
    (void)user;

    if (!wsi) {
        return 0;
    }
    connection = (struct ws_connection *)lws_context_user(lws_get_context(wsi));
    if (!connection) {
        return 0;
    }

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        pthread_mutex_lock(&connection->mutex);
        connection->wsi = wsi;
        connection->connected = 1;
        pthread_cond_broadcast(&connection->changed);
        pthread_mutex_unlock(&connection->mutex);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        struct ws_write_request *request;
        enum lws_write_protocol protocol;
        int written;
        int write_failed;
        size_t request_length;

        pthread_mutex_lock(&connection->mutex);
        if (connection->stopping) {
            pthread_mutex_unlock(&connection->mutex);
            lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
            return -1;
        }

        request = connection->write_head;
        if (!request) {
            pthread_mutex_unlock(&connection->mutex);
            break;
        }

        protocol = request->type == WS_TYPE_TEXT ? LWS_WRITE_TEXT : LWS_WRITE_BINARY;
        request_length = request->length;
        written = lws_write(wsi, request->buffer + LWS_PRE, request_length, protocol);
        write_failed = written < 0 || (size_t)written < request_length;
        connection->write_head = request->next;
        if (!connection->write_head) {
            connection->write_tail = NULL;
        }
        request->result = write_failed ? -1 : (ssize_t)request_length;
        request->error = write_failed ? EIO : 0;
        request->completed = 1;
        pthread_cond_signal(&request->completed_cond);

        if (write_failed) {
            connection->error_code = EIO;
            fail_writes_locked(connection, EIO);
        } else if (connection->write_head) {
            lws_callback_on_writable(wsi);
        }
        pthread_mutex_unlock(&connection->mutex);
        if (write_failed) {
            return -1;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (receive_data(connection, wsi, in, length) != 0) {
            return -1;
        }
        break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
        int need_writable;
        struct lws *client_wsi;

        pthread_mutex_lock(&connection->mutex);
        need_writable = connection->connected &&
                        (connection->stopping || connection->write_head != NULL);
        client_wsi = connection->wsi;
        pthread_mutex_unlock(&connection->mutex);
        if (need_writable && client_wsi) {
            lws_callback_on_writable(client_wsi);
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        pthread_mutex_lock(&connection->mutex);
        connection->error_code = ECONNREFUSED;
        connection->wsi = NULL;
        fail_writes_locked(connection, connection->error_code);
        pthread_cond_broadcast(&connection->changed);
        pthread_mutex_unlock(&connection->mutex);
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        pthread_mutex_lock(&connection->mutex);
        connection->closed = 1;
        connection->wsi = NULL;
        fail_writes_locked(connection, ECONNRESET);
        pthread_cond_broadcast(&connection->changed);
        pthread_mutex_unlock(&connection->mutex);
        break;

    default:
        break;
    }

    return 0;
}

static void *service_thread(void *opaque)
{
    struct ws_connection *connection = (struct ws_connection *)opaque;

    for (;;) {
        int stopping;
        int connected;
        int terminal;
        int service_result;

        pthread_mutex_lock(&connection->mutex);
        stopping = connection->stopping;
        connected = connection->connected;
        terminal = is_terminal(connection);
        pthread_mutex_unlock(&connection->mutex);
        if (terminal || (stopping && !connected)) {
            break;
        }

        service_result = lws_service(connection->context, 0);
        if (service_result < 0) {
            pthread_mutex_lock(&connection->mutex);
            if (!connection->error_code) {
                connection->error_code = EIO;
            }
            connection->closed = 1;
            fail_writes_locked(connection, connection->error_code);
            pthread_cond_broadcast(&connection->changed);
            pthread_mutex_unlock(&connection->mutex);
            break;
        }
    }

    lws_context_destroy(connection->context);
    pthread_mutex_lock(&connection->mutex);
    connection->context = NULL;
    connection->closed = 1;
    if (!connection->error_code && !connection->stopping) {
        connection->error_code = ECONNRESET;
    }
    fail_writes_locked(connection, connection->error_code ? connection->error_code : EPIPE);
    pthread_cond_broadcast(&connection->changed);
    pthread_mutex_unlock(&connection->mutex);
    return NULL;
}

static int parse_url(struct ws_connection *connection, const char *url)
{
    char *mutable_url;
    const char *protocol = NULL;
    const char *address = NULL;
    const char *path = NULL;
    int port = 0;
    size_t path_length;

    mutable_url = strdup(url);
    if (!mutable_url) {
        errno = ENOMEM;
        return -1;
    }

    if (lws_parse_uri(mutable_url, &protocol, &address, &port, &path) != 0 ||
        !protocol || !address || !path || !address[0] || port < 1 || port > 65535) {
        free(mutable_url);
        errno = EINVAL;
        return -1;
    }
    if (strcmp(protocol, "ws") != 0 && strcmp(protocol, "wss") != 0) {
        free(mutable_url);
        errno = EPROTONOSUPPORT;
        return -1;
    }
    if (strchr(path, '#')) {
        free(mutable_url);
        errno = EINVAL;
        return -1;
    }

    connection->use_tls = strcmp(protocol, "wss") == 0;
    connection->port = port;
    connection->address = strdup(address);
    connection->host_header = (char *)malloc(strlen(address) + 32);
    path_length = strlen(path);
    connection->path = (char *)malloc(path_length + (path[0] == '/' ? 1 : 2));
    if (!connection->address || !connection->host_header || !connection->path) {
        free(mutable_url);
        errno = ENOMEM;
        return -1;
    }

    if (strchr(address, ':')) {
        (void)snprintf(connection->host_header, strlen(address) + 32,
                       "[%s]:%d", address, port);
    } else {
        (void)snprintf(connection->host_header, strlen(address) + 32,
                       "%s:%d", address, port);
    }
    if (path[0] == '/') {
        memcpy(connection->path, path, path_length + 1);
    } else {
        connection->path[0] = '/';
        memcpy(connection->path + 1, path, path_length + 1);
    }

    free(mutable_url);
    return 0;
}

ws_connection_t *ws_open(const char *url, int timeout_ms)
{
    struct ws_connection *connection;
    struct lws_context_creation_info context_info;
    struct lws_client_connect_info connect_info;
    struct timespec deadline;
    int wait_result = 0;
    int error;

    if (!url || timeout_ms < 0) {
        errno = EINVAL;
        return NULL;
    }
    if (timeout_ms == 0) {
        timeout_ms = WS_DEFAULT_CONNECT_TIMEOUT_MS;
    }

    connection = (struct ws_connection *)calloc(1, sizeof(*connection));
    if (!connection) {
        errno = ENOMEM;
        return NULL;
    }
    error = pthread_mutex_init(&connection->mutex, NULL);
    if (error) {
        free(connection);
        errno = error;
        return NULL;
    }
    error = pthread_cond_init(&connection->changed, NULL);
    if (error) {
        pthread_mutex_destroy(&connection->mutex);
        free(connection);
        errno = error;
        return NULL;
    }
    if (parse_url(connection, url) != 0) {
        error = errno;
        goto fail_unstarted;
    }

    connection->protocols[0].name = "ws-sync-client";
    connection->protocols[0].callback = ws_callback;
    connection->protocols[0].rx_buffer_size = WS_RX_BUFFER_SIZE;
    connection->protocols[0].tx_packet_size = INT_MAX;
    connection->protocols[1] = (struct lws_protocols)LWS_PROTOCOL_LIST_TERM;

    error = pthread_once(&log_config_once, configure_lws_logging);
    if (error) {
        goto fail_unstarted;
    }
    memset(&context_info, 0, sizeof(context_info));
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = connection->protocols;
    context_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    context_info.user = connection;
    context_info.gid = -1;
    context_info.uid = -1;
    connection->context = lws_create_context(&context_info);
    if (!connection->context) {
        error = EIO;
        goto fail_unstarted;
    }

    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.context = connection->context;
    connect_info.address = connection->address;
    connect_info.port = connection->port;
    connect_info.path = connection->path;
    connect_info.host = connection->host_header;
    connect_info.origin = connection->host_header;
    connect_info.ssl_connection = connection->use_tls ? LCCSCF_USE_SSL : 0;
    connect_info.local_protocol_name = connection->protocols[0].name;
    connect_info.opaque_user_data = connection;
    if (!lws_client_connect_via_info(&connect_info)) {
        error = ECONNREFUSED;
        lws_context_destroy(connection->context);
        connection->context = NULL;
        goto fail_unstarted;
    }

    error = pthread_create(&connection->thread, NULL, service_thread, connection);
    if (error) {
        lws_context_destroy(connection->context);
        connection->context = NULL;
        goto fail_unstarted;
    }
    connection->thread_started = 1;

    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    add_milliseconds(&deadline, timeout_ms);
    pthread_mutex_lock(&connection->mutex);
    while (!connection->connected && !is_terminal(connection)) {
        wait_result = pthread_cond_timedwait(&connection->changed, &connection->mutex,
                                             &deadline);
        if (wait_result != 0) {
            break;
        }
    }
    if (connection->connected) {
        pthread_mutex_unlock(&connection->mutex);
        return connection;
    }
    error = wait_result == ETIMEDOUT ? ETIMEDOUT :
            (connection->error_code ? connection->error_code : ECONNREFUSED);
    pthread_mutex_unlock(&connection->mutex);
    ws_close(connection);
    errno = error;
    return NULL;

fail_unstarted:
    free(connection->address);
    free(connection->host_header);
    free(connection->path);
    pthread_cond_destroy(&connection->changed);
    pthread_mutex_destroy(&connection->mutex);
    free(connection);
    errno = error;
    return NULL;
}

ssize_t ws_write(ws_connection_t *connection, const void *data, size_t length,
                 ws_type_t type)
{
    struct ws_write_request *request;
    int error;
    ssize_t result;

    if (!connection || (!data && length) ||
        (type != WS_TYPE_TEXT && type != WS_TYPE_BINARY)) {
        errno = EINVAL;
        return -1;
    }
    if (length > (size_t)INT_MAX || length > SIZE_MAX - LWS_PRE) {
        errno = EMSGSIZE;
        return -1;
    }

    request = (struct ws_write_request *)calloc(1, sizeof(*request));
    if (!request) {
        errno = ENOMEM;
        return -1;
    }
    request->buffer = (unsigned char *)malloc(LWS_PRE + (length ? length : 1));
    if (!request->buffer) {
        free(request);
        errno = ENOMEM;
        return -1;
    }
    if (length) {
        memcpy(request->buffer + LWS_PRE, data, length);
    }
    request->length = length;
    request->type = type;
    error = pthread_cond_init(&request->completed_cond, NULL);
    if (error) {
        free(request->buffer);
        free(request);
        errno = error;
        return -1;
    }

    pthread_mutex_lock(&connection->mutex);
    if (!connection->connected || connection->stopping || is_terminal(connection)) {
        error = connection->error_code ? connection->error_code : EPIPE;
        pthread_mutex_unlock(&connection->mutex);
        pthread_cond_destroy(&request->completed_cond);
        free(request->buffer);
        free(request);
        errno = error;
        return -1;
    }
    if (connection->write_tail) {
        connection->write_tail->next = request;
    } else {
        connection->write_head = request;
    }
    connection->write_tail = request;
    lws_cancel_service(connection->context);
    while (!request->completed) {
        pthread_cond_wait(&request->completed_cond, &connection->mutex);
    }
    result = request->result;
    error = request->error;
    pthread_mutex_unlock(&connection->mutex);

    pthread_cond_destroy(&request->completed_cond);
    free(request->buffer);
    free(request);
    if (result < 0) {
        errno = error ? error : EIO;
    }
    return result;
}

ssize_t ws_read(ws_connection_t *connection, void *buffer, size_t capacity,
                size_t *message_length, ws_type_t *type, int timeout_ms)
{
    struct ws_message *message;
    struct timespec deadline;
    int wait_result = 0;
    int error;
    ssize_t result;

    if (!connection || !message_length || !type || (!buffer && capacity) || timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }
    if (timeout_ms > 0) {
        (void)clock_gettime(CLOCK_REALTIME, &deadline);
        add_milliseconds(&deadline, timeout_ms);
    }

    pthread_mutex_lock(&connection->mutex);
    while (!connection->read_head && !is_terminal(connection)) {
        wait_result = timeout_ms == 0 ?
                      pthread_cond_wait(&connection->changed, &connection->mutex) :
                      pthread_cond_timedwait(&connection->changed, &connection->mutex,
                                             &deadline);
        if (wait_result != 0) {
            pthread_mutex_unlock(&connection->mutex);
            errno = wait_result == ETIMEDOUT ? ETIMEDOUT : wait_result;
            return -1;
        }
    }

    message = connection->read_head;
    if (!message) {
        error = connection->error_code ? connection->error_code : ECONNRESET;
        pthread_mutex_unlock(&connection->mutex);
        errno = error;
        return -1;
    }
    *message_length = message->length;
    if (capacity < message->length) {
        pthread_mutex_unlock(&connection->mutex);
        errno = EMSGSIZE;
        return -1;
    }
    if (message->length > (size_t)SSIZE_MAX) {
        pthread_mutex_unlock(&connection->mutex);
        errno = EOVERFLOW;
        return -1;
    }

    if (message->length) {
        memcpy(buffer, message->data, message->length);
    }
    *type = message->type;
    result = (ssize_t)message->length;
    connection->read_head = message->next;
    if (!connection->read_head) {
        connection->read_tail = NULL;
    }
    pthread_mutex_unlock(&connection->mutex);
    free(message);
    return result;
}

void ws_close(ws_connection_t *connection)
{
    struct lws_context *context;

    if (!connection) {
        return;
    }

    pthread_mutex_lock(&connection->mutex);
    connection->stopping = 1;
    fail_writes_locked(connection, EPIPE);
    context = connection->context;
    if (context && !is_terminal(connection)) {
        lws_cancel_service(context);
    }
    pthread_cond_broadcast(&connection->changed);
    pthread_mutex_unlock(&connection->mutex);

    if (connection->thread_started) {
        pthread_join(connection->thread, NULL);
    } else if (connection->context) {
        lws_context_destroy(connection->context);
    }

    free_messages(connection);
    free(connection->rx_buffer);
    free(connection->address);
    free(connection->host_header);
    free(connection->path);
    pthread_cond_destroy(&connection->changed);
    pthread_mutex_destroy(&connection->mutex);
    free(connection);
}
