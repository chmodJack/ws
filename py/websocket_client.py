"""WebSocket client supporting text and binary messages.

Install:
    python -m pip install "websockets>=14"

Run against a WebSocket echo server:
    python websocket_client.py ws://127.0.0.1:8765

Interactive commands after connecting:
    <text>         send a text message
    b file <file>  send the contents of a file as a binary message
    b hex <bytes>  send space-separated two-digit hexadecimal bytes
    q              close the connection
"""

from __future__ import annotations

import argparse
import asyncio
import shlex
import string
from pathlib import Path
from typing import Literal

from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import ConnectionClosed


MessageKind = Literal["text", "binary"]


class WebSocketClient:
    """A small WebSocket client with explicit text/binary methods."""

    def __init__(self, uri: str) -> None:
        self.uri = uri
        self._ws: ClientConnection | None = None

    async def connect(self) -> None:
        self._ws = await connect(self.uri)

    def _connection(self) -> ClientConnection:
        if self._ws is None:
            raise RuntimeError("WebSocket is not connected")
        return self._ws

    async def send_text(self, text: str) -> None:
        """Send one WebSocket text message."""
        if not isinstance(text, str):
            raise TypeError("text must be str")
        await self._connection().send(text)

    async def send_binary(self, data: bytes | bytearray | memoryview) -> None:
        """Send one WebSocket binary message."""
        await self._connection().send(bytes(data))

    async def receive(self) -> tuple[MessageKind, str | bytes]:
        """Receive one message and preserve whether it is text or binary."""
        message = await self._connection().recv()
        if isinstance(message, str):
            return "text", message
        return "binary", bytes(message)

    async def close(self) -> None:
        if self._ws is not None:
            await self._ws.close()
            self._ws = None


async def receive_loop(client: WebSocketClient) -> None:
    """Print all incoming messages, distinguishing text and binary frames."""
    try:
        while True:
            kind, payload = await client.receive()
            if kind == "text":
                print(f"[recv text] {payload}")
            else:
                hex_data = " ".join(f"{byte:02X}" for byte in payload)
                print(f"[recv binary] {len(payload)} bytes: {hex_data}")
    except ConnectionClosed:
        print("连接已关闭")


async def input_loop(client: WebSocketClient) -> None:
    """Read commands without blocking the asyncio event loop."""
    print("输入文本直接发送；二进制：b file <文件路径> 或 b hex <两位十六进制> ...；退出：q")
    while True:
        line = await asyncio.to_thread(input, "> ")

        if line == "q":
            return

        # Any input other than the binary prefix is a text message.
        if line != "b" and not line.startswith("b "):
            await client.send_text(line)
            continue

        try:
            parts = shlex.split(line)
            if len(parts) < 3:
                raise ValueError("格式：b file <文件路径> 或 b hex <两位十六进制> ...")

            mode = parts[1]
            if mode == "file":
                if len(parts) != 3:
                    raise ValueError("file 模式只能指定一个文件路径；路径包含空格时请使用引号")
                await client.send_binary(Path(parts[2]).read_bytes())
            elif mode == "hex":
                values = parts[2:]
                if any(
                    len(value) != 2
                    or any(char not in string.hexdigits for char in value)
                    for value in values
                ):
                    raise ValueError("hex 模式的每个值必须是两位十六进制，例如：b hex 01 0A FF")
                await client.send_binary(bytes.fromhex("".join(values)))
            else:
                raise ValueError("二进制模式必须是 file 或 hex")
        except (OSError, ValueError) as exc:
            print(f"输入错误：{exc}")


async def main(uri: str) -> None:
    client = WebSocketClient(uri)
    await client.connect()
    print(f"已连接：{uri}")

    receiver = asyncio.create_task(receive_loop(client))
    try:
        await input_loop(client)
    except (EOFError, KeyboardInterrupt):
        pass
    finally:
        receiver.cancel()
        await client.close()
        await asyncio.gather(receiver, return_exceptions=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Text/binary WebSocket client")
    parser.add_argument("uri", help="WebSocket URI, e.g. ws://127.0.0.1:8765")
    args = parser.parse_args()
    asyncio.run(main(args.uri))
