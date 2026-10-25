"""A WebSocket server that echoes every received message unchanged."""

from __future__ import annotations

import argparse
import asyncio
import logging

from websockets.asyncio.server import ServerConnection, serve


logger = logging.getLogger(__name__)
MAX_MESSAGE_SIZE = 10 * 1024 * 1024


def format_message(message: str | bytes) -> str:
    """Format text for readability and binary data as two-digit hex bytes."""
    if isinstance(message, bytes):
        return message.hex(" ") or "<empty>"
    return repr(message)


async def echo(connection: ServerConnection) -> None:
    """Send each text or binary message back to the client unchanged."""
    request = getattr(connection, "request", None)
    logger.info(
        "client connected: remote=%s local=%s path=%s subprotocol=%s headers=%s",
        connection.remote_address,
        connection.local_address,
        request.path if request is not None else None,
        connection.subprotocol,
        dict(request.headers) if request is not None else {},
    )

    try:
        async for message in connection:
            logger.info("received message: %s", format_message(message))
            await connection.send(message)
    finally:
        logger.info(
            "client disconnected: address=%s code=%s reason=%r",
            connection.remote_address,
            connection.close_code,
            connection.close_reason,
        )


async def run(host: str, port: int) -> None:
    """Run the echo server until interrupted."""
    async with serve(echo, host, port, max_size=MAX_MESSAGE_SIZE):
        print(f"WebSocket echo server listening on ws://{host}:{port}")
        await asyncio.Future()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a WebSocket echo server")
    parser.add_argument("--host", default="0.0.0.0", help="address to listen on")
    parser.add_argument("--port", type=int, default=10000, help="port to listen on")
    return parser.parse_args()


if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    args = parse_args()
    try:
        asyncio.run(run(args.host, args.port))
    except KeyboardInterrupt:
        pass
