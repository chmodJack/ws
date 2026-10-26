# libwebsockets C 客户端封装

封装提供 `ws_open`、`ws_write`、`ws_read`、`ws_close`，调用方不用编写 libwebsockets 回调或事件循环代码。

## 编译

需要安装 libwebsockets 开发包和 pthread：

```sh
cmake -S . -B build
cmake --build build
```

也可直接编译示例：

```sh
cc -std=c11 -Wall -Wextra -Wpedantic websocket_client.c ws_client.c \
   $(pkg-config --cflags --libs libwebsockets) -pthread -o lws_client
```

## API

```c
#include <errno.h>
#include <stdio.h>
#include "ws_client.h"

ws_connection_t *connection = ws_open("wss://example.com/ws", 5000);
if (!connection) {
    perror("ws_open");
    return 1;
}

ws_write(connection, "hello", 5, WS_TYPE_TEXT);
ws_write(connection, binary_data, binary_length, WS_TYPE_BINARY);

size_t message_length;
ws_type_t type;
ssize_t received = ws_read(connection, buffer, sizeof(buffer),
                           &message_length, &type, 1000);
if (received >= 0) {
    /* type is WS_TYPE_TEXT or WS_TYPE_BINARY; received is exact byte count. */
} else if (errno == EMSGSIZE) {
    /* message_length reports required capacity; the message remains queued. */
}

ws_close(connection);
```

- `ws_open(url, timeout_ms)` 支持 `ws://` 和 `wss://`；连接成功返回句柄，失败返回 `NULL` 并设置 `errno`。超时参数为 0 时使用 10 秒。
- `ws_write(connection, data, length, type)` 发送一条完整消息。返回写入的字节数，失败返回 -1 并设置 `errno`。返回成功表示 libwebsockets 接受了消息，不表示对端已收到。
- `ws_read(connection, buffer, capacity, &message_length, &type, timeout_ms)` 返回一条完整消息的字节数。超时参数为 0 时无限等待；有限超时返回 -1 并设置 `errno=ETIMEDOUT`。缓冲区太小时返回 `EMSGSIZE`，`message_length` 提供所需容量，消息保留在队列中。
- `ws_close(connection)` 关闭连接并释放资源；`NULL` 句柄可安全传入。

读写调用可从一个应用线程并发执行。关闭句柄时，调用方应先停止对该句柄的其他并发调用。文本消息使用 UTF-8；二进制按原始字节传输。读接口要求完整消息能放入调用方提供的缓冲区。

## 示例

```sh
./build/lws_client ws://127.0.0.1:9001/echo \
    --text 'hello from C' \
    --binary ./payload.bin
```

命令行程序展示如何使用封装发送文本 / 二进制消息并读取服务端回复。

## 验证

测试使用 Python 3 标准库启动本地 WebSocket 服务，验证文本和二进制收发、消息边界和超时行为：

```sh
python3 tests/test_client.py ./build/lws_client
```
