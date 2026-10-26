#define _POSIX_C_SOURCE 200809L

#include "ws_client.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_READ_CAPACITY 4096

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
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
            "示例: %s ws://127.0.0.1:9001/echo --text hello --binary payload.bin\n",
            program, program);
}

static int read_file(const char *path, unsigned char **data, size_t *length)
{
    FILE *file = fopen(path, "rb");
    long file_size;
    size_t count;
    unsigned char *buffer = NULL;

    if (!file) {
        perror(path);
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "无法获取文件大小: %s\n", path);
        fclose(file);
        return -1;
    }
    if ((unsigned long long)file_size > (unsigned long long)SIZE_MAX) {
        fprintf(stderr, "文件过大: %s\n", path);
        fclose(file);
        return -1;
    }

    if (file_size > 0) {
        buffer = (unsigned char *)malloc((size_t)file_size);
        if (!buffer) {
            fclose(file);
            return -1;
        }
        count = fread(buffer, 1, (size_t)file_size, file);
        if (count != (size_t)file_size) {
            fprintf(stderr, "读取文件失败: %s\n", path);
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

static void print_text(const unsigned char *data, size_t length)
{
    size_t i;
    putchar('"');
    for (i = 0; i < length; ++i) {
        unsigned char c = data[i];
        switch (c) {
        case '\\': fputs("\\\\", stdout); break;
        case '"': fputs("\\\"", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (c >= 0x20 && c != 0x7f) {
                putchar((int)c);
            } else {
                printf("\\x%02x", c);
            }
            break;
        }
    }
    putchar('"');
}

static void print_message(ws_type_t type, const unsigned char *data, size_t length)
{
    size_t i;
    if (type == WS_TYPE_TEXT) {
        printf("[recv][text] len=%zu, data=", length);
        print_text(data, length);
        putchar('\n');
        return;
    }

    printf("[recv][binary] len=%zu, data=", length);
    for (i = 0; i < length; ++i) {
        printf("%02x%s", data[i], i + 1 == length ? "" : " ");
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    ws_connection_t *connection;
    char *text = NULL;
    unsigned char *binary = NULL;
    size_t binary_length = 0;
    unsigned char *read_buffer;
    size_t read_capacity = INITIAL_READ_CAPACITY;
    int has_binary = 0;
    int i;
    int exit_code = EXIT_SUCCESS;

    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            char *replacement = strdup(argv[++i]);
            if (!replacement) {
                perror("strdup");
                exit_code = EXIT_FAILURE;
                goto cleanup;
            }
            free(text);
            text = replacement;
        } else if (strcmp(argv[i], "--binary") == 0 && i + 1 < argc) {
            unsigned char *replacement = NULL;
            size_t replacement_length = 0;
            if (read_file(argv[++i], &replacement, &replacement_length) != 0) {
                exit_code = EXIT_FAILURE;
                goto cleanup;
            }
            free(binary);
            binary = replacement;
            binary_length = replacement_length;
            has_binary = 1;
        } else {
            fprintf(stderr, "无效选项或缺少参数: %s\n", argv[i]);
            print_usage(argv[0]);
            exit_code = EXIT_FAILURE;
            goto cleanup;
        }
    }

    printf("[连接] 正在连接 %s\n", argv[1]);
    connection = ws_open(argv[1], 10000);
    if (!connection) {
        perror("ws_open");
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }
    puts("[连接] 已连接到 WebSocket 服务器");

    if (text) {
        ssize_t sent = ws_write(connection, text, strlen(text), WS_TYPE_TEXT);
        if (sent < 0) {
            perror("ws_write(text)");
            exit_code = EXIT_FAILURE;
            ws_close(connection);
            goto cleanup;
        }
        printf("[send][text] %zd 字节\n", sent);
    }
    if (has_binary) {
        ssize_t sent = ws_write(connection, binary, binary_length, WS_TYPE_BINARY);
        if (sent < 0) {
            perror("ws_write(binary)");
            exit_code = EXIT_FAILURE;
            ws_close(connection);
            goto cleanup;
        }
        printf("[send][binary] %zd 字节\n", sent);
    }

    read_buffer = (unsigned char *)malloc(read_capacity);
    if (!read_buffer) {
        perror("malloc");
        exit_code = EXIT_FAILURE;
        ws_close(connection);
        goto cleanup;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!stop_requested) {
        size_t message_length = 0;
        ws_type_t type;
        ssize_t received = ws_read(connection, read_buffer, read_capacity,
                                   &message_length, &type, 1000);
        if (received >= 0) {
            print_message(type, read_buffer, (size_t)received);
            fflush(stdout);
            continue;
        }
        if (errno == EMSGSIZE && message_length > read_capacity) {
            unsigned char *replacement = (unsigned char *)realloc(read_buffer,
                                                                   message_length);
            if (!replacement) {
                perror("realloc");
                exit_code = EXIT_FAILURE;
                break;
            }
            read_buffer = replacement;
            read_capacity = message_length;
            /* ws_read kept the complete message queued; retry with enough space. */
            continue;
        }
        if (errno == ETIMEDOUT) {
            continue;
        }
        if (errno == ECONNRESET || errno == EPIPE) {
            break;
        }
        perror("ws_read");
        exit_code = EXIT_FAILURE;
        break;
    }

    free(read_buffer);
    ws_close(connection);

cleanup:
    free(text);
    free(binary);
    return exit_code;
}
