#define _POSIX_C_SOURCE 200809L

#include <libwebsockets.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define RX_BUFFER_SIZE 4096
#define DEFAULT_WS_PORT 80
#define DEFAULT_WSS_PORT 443

struct endpoint {
    char *host;
    char *path;
    int port;
    int use_ssl;
};

struct client_app {
    struct lws_context *context;
    struct lws *wsi;

    char *text;
    size_t text_len;
    int has_text;
    int text_done;

    unsigned char *binary;
    size_t binary_len;
    int has_binary;
    int binary_done;

    unsigned char *rx_buffer;
    size_t rx_length;
    size_t rx_capacity;
    int rx_binary;
    int rx_in_progress;

    int closed;
    int connection_error;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "用法: %s <ws://host[:port]/path 或 wss://...> [选项]\n"
            "选项:\n"
            "  --text TEXT       发送一个文本消息\n"
            "  --binary FILE     发送 FILE 的二进制内容\n"
            "  --help            显示帮助\n\n"
            "示例:\n"
            "  %s ws://127.0.0.1:9001/echo --text hello --binary payload.bin\n",
            program, program);
}

static char *duplicate_range(const char *begin, size_t length)
{
    char *result = (char *)malloc(length + 1);
    if (!result) {
        return NULL;
    }

    memcpy(result, begin, length);
    result[length] = '\0';
    return result;
}

static int parse_port_range(const char *begin, const char *end, int *port)
{
    char *value;
    char *value_end = NULL;
    long parsed;
    size_t length;

    if (!begin || !end || begin >= end) {
        return -1;
    }

    length = (size_t)(end - begin);
    value = duplicate_range(begin, length);
    if (!value) {
        return -1;
    }

    errno = 0;
    parsed = strtol(value, &value_end, 10);
    if (errno || value_end == value || *value_end != '\0' ||
        parsed < 1 || parsed > 65535) {
        free(value);
        return -1;
    }

    free(value);
    *port = (int)parsed;
    return 0;
}

static int parse_endpoint(const char *uri, struct endpoint *endpoint)
{
    const char *authority_begin;
    const char *authority_end;
    const char *path_begin;
    const char *colon;
    const char *scheme_end;
    size_t authority_length;

    memset(endpoint, 0, sizeof(*endpoint));

    if (strncmp(uri, "ws://", 5) == 0) {
        endpoint->use_ssl = 0;
        endpoint->port = DEFAULT_WS_PORT;
        scheme_end = uri + 5;
    } else if (strncmp(uri, "wss://", 6) == 0) {
        endpoint->use_ssl = 1;
        endpoint->port = DEFAULT_WSS_PORT;
        scheme_end = uri + 6;
    } else {
        fprintf(stderr, "错误: 地址必须以 ws:// 或 wss:// 开头\n");
        return -1;
    }

    authority_begin = scheme_end;
    path_begin = strchr(authority_begin, '/');
    authority_end = path_begin ? path_begin : uri + strlen(uri);
    authority_length = (size_t)(authority_end - authority_begin);

    if (authority_length == 0) {
        fprintf(stderr, "错误: 地址缺少主机名\n");
        return -1;
    }

    /* 支持 [IPv6]:port；普通主机名支持 host:port。 */
    if (*authority_begin == '[') {
        const char *closing_bracket = memchr(authority_begin, ']', authority_length);
        if (!closing_bracket || closing_bracket == authority_begin + 1) {
            fprintf(stderr, "错误: IPv6 主机地址格式无效\n");
            return -1;
        }

        endpoint->host = duplicate_range(authority_begin + 1,
                                          (size_t)(closing_bracket - authority_begin - 1));
        if (!endpoint->host) {
            return -1;
        }

        if (closing_bracket + 1 < authority_end) {
            if (closing_bracket[1] != ':' ||
                parse_port_range(closing_bracket + 2, authority_end, &endpoint->port) != 0) {
                fprintf(stderr, "错误: 端口格式无效\n");
                free(endpoint->host);
                endpoint->host = NULL;
                return -1;
            }
        }
    } else {
        colon = memchr(authority_begin, ':', authority_length);
        if (colon) {
            endpoint->host = duplicate_range(authority_begin,
                                              (size_t)(colon - authority_begin));
            if (!endpoint->host || parse_port_range(colon + 1, authority_end, &endpoint->port) != 0) {
                fprintf(stderr, "错误: 主机名或端口格式无效\n");
                free(endpoint->host);
                endpoint->host = NULL;
                return -1;
            }
        } else {
            endpoint->host = duplicate_range(authority_begin, authority_length);
            if (!endpoint->host) {
                return -1;
            }
        }
    }

    if (!endpoint->host[0]) {
        fprintf(stderr, "错误: 主机名不能为空\n");
        free(endpoint->host);
        endpoint->host = NULL;
        return -1;
    }

    endpoint->path = strdup(path_begin ? path_begin : "/");
    if (!endpoint->path) {
        free(endpoint->host);
        endpoint->host = NULL;
        return -1;
    }

    return 0;
}

static int read_binary_file(const char *filename, unsigned char **data, size_t *length)
{
    FILE *file;
    long file_size;
    size_t bytes_read;
    unsigned char *buffer;

    file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "无法打开二进制文件 %s: %s\n", filename, strerror(errno));
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "无法读取二进制文件大小 %s\n", filename);
        fclose(file);
        return -1;
    }

    if ((unsigned long long)file_size > (unsigned long long)SIZE_MAX) {
        fprintf(stderr, "二进制文件过大: %s\n", filename);
        fclose(file);
        return -1;
    }

    buffer = NULL;
    if (file_size > 0) {
        buffer = (unsigned char *)malloc((size_t)file_size);
        if (!buffer) {
            fprintf(stderr, "内存分配失败\n");
            fclose(file);
            return -1;
        }

        bytes_read = fread(buffer, 1, (size_t)file_size, file);
        if (bytes_read != (size_t)file_size) {
            fprintf(stderr, "读取二进制文件失败: %s\n", filename);
            free(buffer);
            fclose(file);
            return -1;
        }
    }

    fclose(file);
    *data = buffer;
    *length = (size_t)file_size;
    return 0;
}

static void print_escaped_text(const unsigned char *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        unsigned char c = data[i];
        switch (c) {
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (c >= 0x20 && c != 0x7f) {
                fputc((int)c, stdout);
            } else {
                fprintf(stdout, "\\x%02x", c);
            }
            break;
        }
    }
}

static void print_binary_hex(const unsigned char *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        fprintf(stdout, "%02x%s", data[i], (i + 1 == length) ? "" : " ");
    }
}

static int send_next_message(struct client_app *app, struct lws *wsi)
{
    unsigned char *buffer;
    const unsigned char *source;
    int *done;
    size_t total_length;
    enum lws_write_protocol write_type;
    int written;

    if (app->has_text && !app->text_done) {
        source = (const unsigned char *)app->text;
        done = &app->text_done;
        total_length = app->text_len;
        write_type = LWS_WRITE_TEXT;
    } else if (app->has_binary && !app->binary_done) {
        source = app->binary;
        done = &app->binary_done;
        total_length = app->binary_len;
        write_type = LWS_WRITE_BINARY;
    } else {
        return 0;
    }

    /* lws_write 返回 int；同时保证 LWS_PRE 和空消息的缓冲区分配安全。 */
    if (total_length > INT_MAX || total_length > SIZE_MAX - LWS_PRE - 1) {
        fprintf(stderr, "消息过大，无法通过单个 WebSocket 帧发送\n");
        return -1;
    }

    buffer = (unsigned char *)malloc(LWS_PRE + total_length + 1);
    if (!buffer) {
        fprintf(stderr, "发送缓冲区分配失败\n");
        return -1;
    }

    if (total_length > 0) {
        memcpy(buffer + LWS_PRE, source, total_length);
    }

    /* 一次写入完整消息，默认 FIN=1，不发送 continuation 帧。 */
    written = lws_write(wsi, buffer + LWS_PRE, total_length, write_type);
    free(buffer);
    if (written < 0 || (size_t)written < total_length) {
        fprintf(stderr, "lws_write 失败: expected=%zu, actual=%d\n",
                total_length, written);
        return -1;
    }

    *done = 1;
    printf("[send][%s] %zu 字节\n",
           write_type == LWS_WRITE_TEXT ? "text" : "binary", total_length);

    if ((app->has_text && !app->text_done) || (app->has_binary && !app->binary_done)) {
        lws_callback_on_writable(wsi);
    }

    return 0;
}

/* 以下两个函数只接收完整消息，业务处理可以放在这里。 */
static void on_text_message(const unsigned char *data, size_t length)
{
    printf("[recv][text] len=%zu, data=\"", length);
    print_escaped_text(data, length);
    puts("\"");
}

static void on_binary_message(const unsigned char *data, size_t length)
{
    printf("[recv][binary] len=%zu, data=", length);
    print_binary_hex(data, length);
    putchar('\n');
}

static int receive_message(struct client_app *app, struct lws *wsi,
                           const void *data, size_t length)
{
    size_t required;
    size_t capacity;
    unsigned char *buffer;

    if (!app->rx_in_progress) {
        app->rx_binary = lws_frame_is_binary(wsi);
        app->rx_in_progress = 1;
    }

    if (length > SIZE_MAX - app->rx_length - 1) {
        fprintf(stderr, "接收消息过大\n");
        return -1;
    }
    required = app->rx_length + length + 1;
    if (required > app->rx_capacity) {
        capacity = app->rx_capacity ? app->rx_capacity : RX_BUFFER_SIZE;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        buffer = (unsigned char *)realloc(app->rx_buffer, capacity);
        if (!buffer) {
            fprintf(stderr, "接收缓冲区分配失败\n");
            return -1;
        }
        app->rx_buffer = buffer;
        app->rx_capacity = capacity;
    }

    if (length > 0) {
        memcpy(app->rx_buffer + app->rx_length, data, length);
    }
    app->rx_length += length;

    /* 库可能分多次交付同一消息，只在完整消息到齐后通知业务层。 */
    if (!lws_is_final_fragment(wsi) || lws_remaining_packet_payload(wsi) != 0) {
        return 0;
    }

    app->rx_buffer[app->rx_length] = '\0';
    if (app->rx_binary) {
        on_binary_message(app->rx_buffer, app->rx_length);
    } else {
        on_text_message(app->rx_buffer, app->rx_length);
    }
    fflush(stdout);
    app->rx_length = 0;
    app->rx_in_progress = 0;
    return 0;
}

static int callback_websocket(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len)
{
    struct client_app *app = (struct client_app *)lws_get_opaque_user_data(wsi);
    (void)user;

    if (!app) {
        return 0;
    }

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        app->wsi = wsi;
        printf("[连接] 已连接到 WebSocket 服务器\n");
        if (app->has_text || app->has_binary) {
            lws_callback_on_writable(wsi);
        }
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (send_next_message(app, wsi) != 0) {
            app->connection_error = 1;
            return -1;
        }
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (receive_message(app, wsi, in, len) != 0) {
            app->connection_error = 1;
            return -1;
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fprintf(stderr, "[连接错误] %s\n", in ? (const char *)in : "未知错误");
        app->connection_error = 1;
        app->wsi = NULL;
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        printf("[连接] 连接已关闭\n");
        app->closed = 1;
        app->wsi = NULL;
        break;

    default:
        break;
    }

    return 0;
}

static int parse_options(int argc, char **argv, struct client_app *app)
{
    int i;

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }

        if (strcmp(argv[i], "--text") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--text 缺少参数\n");
                return -1;
            }
            free(app->text);
            app->text = strdup(argv[++i]);
            if (!app->text) {
                fprintf(stderr, "内存分配失败\n");
                return -1;
            }
            app->text_len = strlen(app->text);
            app->has_text = 1;
            continue;
        }

        if (strcmp(argv[i], "--binary") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--binary 缺少文件名\n");
                return -1;
            }
            free(app->binary);
            app->binary = NULL;
            app->binary_len = 0;
            if (read_binary_file(argv[++i], &app->binary, &app->binary_len) != 0) {
                return -1;
            }
            app->has_binary = 1;
            continue;
        }

        fprintf(stderr, "未知选项: %s\n", argv[i]);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct endpoint endpoint;
    struct client_app app;
    struct lws_context_creation_info context_info;
    struct lws_client_connect_info connect_info;
    struct lws_protocols protocols[] = {
        {
            .name = "lws-c-client",
            .callback = callback_websocket,
            .rx_buffer_size = RX_BUFFER_SIZE
        },
        LWS_PROTOCOL_LIST_TERM
    };
    int option_result;
    int service_result;
    int exit_code = EXIT_FAILURE;

    memset(&endpoint, 0, sizeof(endpoint));
    memset(&app, 0, sizeof(app));

    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if (parse_endpoint(argv[1], &endpoint) != 0) {
        return EXIT_FAILURE;
    }

    option_result = parse_options(argc, argv, &app);
    if (option_result != 0) {
        free(endpoint.host);
        free(endpoint.path);
        free(app.text);
        free(app.binary);
        return option_result > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* 发送缓冲大小覆盖整个消息，接收缓冲大小只影响库的单次回调。 */
    protocols[0].tx_packet_size = app.text_len > app.binary_len ?
                                 app.text_len : app.binary_len;
    if (protocols[0].tx_packet_size == 0) {
        protocols[0].tx_packet_size = 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 仅保留警告和错误；应用层的连接、收发信息由本程序打印。 */
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    memset(&context_info, 0, sizeof(context_info));
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = protocols;
    context_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    context_info.gid = -1;
    context_info.uid = -1;

    app.context = lws_create_context(&context_info);
    if (!app.context) {
        fprintf(stderr, "无法创建 libwebsockets context\n");
        goto cleanup;
    }

    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.context = app.context;
    connect_info.address = endpoint.host;
    connect_info.port = endpoint.port;
    connect_info.path = endpoint.path;
    connect_info.host = endpoint.host;
    connect_info.ssl_connection = endpoint.use_ssl ? LCCSCF_USE_SSL : 0;
    /* 不强制服务端协商子协议，但将连接绑定到本地回调协议。 */
    connect_info.local_protocol_name = protocols[0].name;
    connect_info.opaque_user_data = &app;
    connect_info.pwsi = &app.wsi;

    app.wsi = lws_client_connect_via_info(&connect_info);
    if (!app.wsi) {
        fprintf(stderr, "无法发起 WebSocket 连接\n");
        goto cleanup_context;
    }

    printf("[连接] 正在连接 %s://%s:%d%s\n",
           endpoint.use_ssl ? "wss" : "ws", endpoint.host, endpoint.port, endpoint.path);

    while (!stop_requested && !app.closed && !app.connection_error) {
        service_result = lws_service(app.context, 1000);
        if (service_result < 0) {
            fprintf(stderr, "lws_service 失败: %d\n", service_result);
            app.connection_error = 1;
            break;
        }
    }

    if (stop_requested && app.wsi) {
        lws_set_timeout(app.wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_SYNC);
    }

    exit_code = app.connection_error ? EXIT_FAILURE : EXIT_SUCCESS;

cleanup_context:
    lws_context_destroy(app.context);
cleanup:
    free(endpoint.host);
    free(endpoint.path);
    free(app.text);
    free(app.binary);
    free(app.rx_buffer);
    return exit_code;
}
