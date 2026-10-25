"""Check WebSocket frames and complete-message delivery using the standard library."""

import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import unittest


CLIENT = Path(sys.argv[1] if len(sys.argv) > 1 else "build/lws_client").resolve()
sys.argv = sys.argv[:1]


def read_exact(reader, length):
    data = reader.read(length)
    if len(data) != length:
        raise EOFError("connection ended before the complete frame arrived")
    return data


def read_client_frame(reader):
    first, second = read_exact(reader, 2)
    if first & 0x70 or not second & 0x80:
        raise AssertionError("expected a masked frame without extensions")
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", read_exact(reader, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", read_exact(reader, 8))[0]
    mask = read_exact(reader, 4)
    payload = read_exact(reader, length)
    payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    return bool(first & 0x80), first & 0x0F, payload


def encode_server_frame(final, opcode, payload):
    length = len(payload)
    header = bytes([(0x80 if final else 0) | opcode])
    if length < 126:
        header += bytes([length])
    elif length <= 65535:
        header += bytes([126]) + struct.pack("!H", length)
    else:
        header += bytes([127]) + struct.pack("!Q", length)
    return header + payload


def serve_connection(listener, expected_messages, response_frames):
    connection, _ = listener.accept()
    with connection:
        connection.settimeout(10)
        with connection.makefile("rb") as reader:
            request = reader.readline()
            if request != b"GET /echo HTTP/1.1\r\n":
                raise AssertionError(f"unexpected request: {request!r}")
            headers = {}
            while True:
                line = reader.readline()
                if line == b"\r\n":
                    break
                if not line:
                    raise EOFError("incomplete handshake")
                name, value = line.decode("ascii").split(":", 1)
                headers[name.lower()] = value.strip()
            key = headers["sec-websocket-key"]
            accept = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
            ).digest()).decode("ascii")
            connection.sendall((
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
            ).encode("ascii"))

            for opcode, payload in expected_messages:
                final, actual_opcode, actual_payload = read_client_frame(reader)
                if not final or actual_opcode != opcode or actual_payload != payload:
                    raise AssertionError(
                        f"expected one complete frame: opcode={opcode}, len={len(payload)}; "
                        f"got FIN={final}, opcode={actual_opcode}, len={len(actual_payload)}"
                    )

            for final, opcode, payload in response_frames:
                connection.sendall(encode_server_frame(final, opcode, payload))
            connection.sendall(encode_server_frame(True, 8, struct.pack("!H", 1000)))

            while True:
                _, opcode, _ = read_client_frame(reader)
                if opcode == 8:
                    return
                if opcode != 10:
                    raise AssertionError(f"unexpected extra client frame: opcode={opcode}")


class ClientMessagesTest(unittest.TestCase):
    def run_client(self, text=None, binary=None, responses=None):
        expected = []
        with tempfile.TemporaryDirectory(prefix="lws-client-test-") as directory:
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen(1)
                listener.settimeout(10)
                port = listener.getsockname()[1]
                command = [str(CLIENT), f"ws://127.0.0.1:{port}/echo"]
                if text is not None:
                    command += ["--text", text]
                    expected.append((1, text.encode("utf-8")))
                if binary is not None:
                    filename = Path(directory) / "payload.bin"
                    filename.write_bytes(binary)
                    command += ["--binary", str(filename)]
                    expected.append((2, binary))
                if responses is None:
                    responses = [(True, opcode, payload) for opcode, payload in expected]

                # libwebsockets may use proxy environment variables for localhost.
                environment = {key: value for key, value in os.environ.items()
                               if not key.lower().endswith("_proxy")}
                with ThreadPoolExecutor(max_workers=1) as executor:
                    server = executor.submit(serve_connection, listener, expected, responses)
                    completed = subprocess.run(command, capture_output=True, text=True,
                                               env=environment, timeout=15)
                    server.result(timeout=12)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        return [line for line in completed.stdout.splitlines() if line.startswith("[recv]")]

    def assert_messages(self, actual, messages):
        expected = []
        for opcode, payload in messages:
            if opcode == 1:
                text = payload.decode("utf-8")
                text = (text.replace("\\", "\\\\").replace('"', '\\"')
                        .replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t"))
                expected.append(f'[recv][text] len={len(payload)}, data="{text}"')
            else:
                expected.append(f"[recv][binary] len={len(payload)}, data={payload.hex(' ')}")
        self.assertEqual(actual, expected)

    def test_large_text_and_binary_use_single_frames(self):
        text = "中文🙂" * 2048 + '\n"\\尾'
        binary = bytes(range(256)) * 300  # Includes NUL and requires a 64-bit frame length.
        self.assert_messages(self.run_client(text, binary),
                             [(1, text.encode("utf-8")), (2, binary)])

    def test_empty_messages(self):
        self.assert_messages(self.run_client("", b""), [(1, b""), (2, b"")])

    def test_buffer_size_boundaries(self):
        for size in (4095, 4096, 4097):
            with self.subTest(size=size):
                text = "x" * size
                binary = b"\x00\xff" * (size // 2) + b"\x01" * (size % 2)
                self.assert_messages(self.run_client(text, binary),
                                     [(1, text.encode("ascii")), (2, binary)])

    def test_server_fragments_are_delivered_as_complete_messages(self):
        binary = bytes(range(256)) * 40
        text = "中文🙂".encode("utf-8")
        frames = [
            (False, 1, b""),
            (False, 0, text[:1]),
            (True, 9, b"ping"),
            (True, 0, text[1:]),
            (False, 2, binary[:5000]),
            (True, 0, binary[5000:]),
            (True, 1, b"next"),
            (True, 2, b""),
        ]
        self.assert_messages(self.run_client(responses=frames),
                             [(1, text), (2, binary), (1, b"next"), (2, b"")])

    def test_incomplete_message_is_not_delivered(self):
        self.assertEqual(self.run_client(responses=[(False, 2, b"incomplete")]), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
