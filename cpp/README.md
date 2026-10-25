# libwebsockets C WebSocket 客户端

这个示例使用 C 和 libwebsockets 实现 WebSocket 客户端：

- `--text` 通过 `LWS_WRITE_TEXT` 发送文本消息；
- `--binary` 通过 `LWS_WRITE_BINARY` 发送二进制消息；
- 每条消息通过一次 `lws_write()` 发送为单个 WebSocket 帧，不主动分片；
- 接收时使用 `lws_frame_is_binary()` 区分文本消息和二进制消息；
- 收齐整条消息后才调用 `on_text_message()` 或 `on_binary_message()`，每条消息只处理、打印一次；
- 文本按 UTF-8 显示，控制字符转义；二进制数据以十六进制打印。

发送和接收均按完整消息处理。libwebsockets 仍可能分多次回调交付一条消息，客户端会在内部合并；接收缓冲区的 4096 字节大小不限制消息长度。对端如何封装 WebSocket 帧以及底层 TCP 如何分段由对端和网络栈决定。

## 编译

系统需要安装 libwebsockets 的开发包，并提供 `pkg-config` 配置：

```sh
cmake -S . -B build
cmake --build build
```

也可以直接编译：

```sh
cc -std=c11 -Wall -Wextra -Wpedantic websocket_client.c \
   $(pkg-config --cflags --libs libwebsockets) -o lws_client
```

## 运行

```sh
./build/lws_client ws://127.0.0.1:9001/echo \
    --text 'hello from C' \
    --binary ./payload.bin
```

支持 `ws://` 和 `wss://`，未指定端口时分别使用 80 和 443。客户端会持续运行以接收服务端消息，按 `Ctrl-C` 退出。

## 验证

测试使用 Python 3 标准库启动本地服务，检查发送帧的 FIN、opcode 和完整内容，并验证接收消息合并、空消息及缓冲区边界：

```sh
python3 tests/test_client.py ./build/lws_client
```
