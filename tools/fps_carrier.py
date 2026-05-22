#!/usr/bin/env python3

import argparse
import asyncio
import hashlib
import os
import random
import ssl
import struct
import subprocess
import sys
import time
from pathlib import Path

import websockets
from websockets.exceptions import ConnectionClosed


REQUEST_MAGIC = b"FPSCARR1"
REQUEST_HEADER = struct.Struct("!8sQI")


def log(message):
    print(f"fps_carrier: {message}", file=sys.stderr, flush=True)


def parse_endpoint(value):
    host, separator, port_text = value.rpartition(":")
    if not separator or not host or not port_text:
        raise argparse.ArgumentTypeError("endpoint must be HOST:PORT")
    try:
        port = int(port_text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("endpoint port must be an integer") from error
    if port <= 0 or port > 65535:
        raise argparse.ArgumentTypeError("endpoint port must be in 1..65535")
    return host, port


def find_executable(name):
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        if not directory:
            continue
        candidate = Path(directory) / name
        if candidate.exists() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def deterministic_bytes(seed_material, size):
    output = bytearray()
    counter = 0
    while len(output) < size:
        output.extend(hashlib.sha256(seed_material + counter.to_bytes(4, "big")).digest())
        counter += 1
    return bytes(output[:size])


def clamp(value, lower, upper):
    return max(lower, min(upper, value))


def planned_frame_size(bps, frame_rate, min_size, max_size, jitter, rng, header_size):
    base = max(header_size, int(round(float(bps) / float(frame_rate))))
    if jitter > 0.0:
        factor = 1.0 + rng.uniform(-jitter, jitter)
        base = int(round(float(base) * factor))
    return clamp(base, max(min_size, header_size), max_size)


def build_request_payload(seq, total_size, seed):
    total_size = max(total_size, REQUEST_HEADER.size)
    body_size = total_size - REQUEST_HEADER.size
    seed_material = f"client:{seed}:{seq}".encode("ascii")
    body = deterministic_bytes(seed_material, body_size)
    return REQUEST_HEADER.pack(REQUEST_MAGIC, seq, body_size) + body


def parse_request_payload(payload):
    if not isinstance(payload, bytes):
        raise ValueError("expected binary WebSocket request frame")
    if len(payload) < REQUEST_HEADER.size:
        raise ValueError("request frame too small")
    magic, seq, body_size = REQUEST_HEADER.unpack_from(payload)
    if magic != REQUEST_MAGIC:
        raise ValueError("invalid request frame magic")
    if body_size != len(payload) - REQUEST_HEADER.size:
        raise ValueError("invalid request frame body size")
    return seq


def ensure_self_signed_cert(cert, key, generate):
    cert_path = Path(cert)
    key_path = Path(key)
    if cert_path.exists() and key_path.exists():
        return
    if not generate:
        raise FileNotFoundError("cert/key missing and --generate-self-signed is not set")
    openssl = find_executable("openssl")
    if openssl is None:
        raise RuntimeError("openssl executable is required for --generate-self-signed")
    cert_path.parent.mkdir(parents=True, exist_ok=True)
    key_path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            openssl,
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            str(key_path),
            "-out",
            str(cert_path),
            "-subj",
            "/CN=localhost",
            "-days",
            "7",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=True,
    )


async def handle_origin_connection(websocket, path, expected_path, max_frame_size):
    if path != expected_path:
        await websocket.close(code=1008, reason="unexpected path")
        return

    try:
        async for message in websocket:
            payload_size = len(message) if isinstance(message, bytes) else len(message.encode("utf-8"))
            if payload_size > max_frame_size:
                await websocket.close(code=1009, reason="message too large")
                return
            await websocket.send(message)
    except (ConnectionClosed, ValueError) as error:
        log(f"event=origin_connection_closed error={error.__class__.__name__}")


async def handle_origin_http_request(path, request_headers):
    if request_headers.get("Upgrade", "").lower() == "websocket":
        return None
    body = f"fps_carrier echo origin path={path}\n".encode("utf-8")
    headers = [
        ("Content-Type", "text/plain; charset=utf-8"),
        ("Content-Length", str(len(body))),
        ("Connection", "close"),
    ]
    return 200, headers, body


async def run_origin_async(args):
    host, port = parse_endpoint(args.listen)
    ensure_self_signed_cert(args.cert, args.key, args.generate_self_signed)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)

    async def handler(websocket, path):
        await handle_origin_connection(websocket, path, args.path, args.max_frame_size)

    async with websockets.serve(
        handler,
        host,
        port,
        ssl=context,
        compression=None,
        max_size=None,
        ping_interval=20,
        ping_timeout=20,
        process_request=handle_origin_http_request,
    ):
        print(f"READY origin listen={host}:{port} path={args.path}", flush=True)
        await asyncio.Future()


def run_origin(args):
    return asyncio.run(run_origin_async(args))


def should_stop(args, frame_count, deadline):
    if args.frames is not None and frame_count >= args.frames:
        return True
    if deadline is not None and time.monotonic() >= deadline:
        return True
    return False


def touch_ready_file(path):
    if not path:
        return
    ready_path = Path(path)
    ready_path.parent.mkdir(parents=True, exist_ok=True)
    ready_path.write_text("ready\n", encoding="utf-8")


def client_ssl_context(args):
    if args.ca_file:
        return ssl.create_default_context(cafile=args.ca_file)
    return ssl._create_unverified_context()


async def run_client_connection(args, frame_count, ready_emitted, deadline):
    host, port = parse_endpoint(args.connect)
    uri = f"wss://{host}:{port}{args.path}"
    rng = random.Random(args.seed + frame_count)
    interval = 1.0 / float(args.frame_rate)
    next_send = time.monotonic()

    async with websockets.connect(
        uri,
        ssl=client_ssl_context(args),
        server_hostname=args.server_name,
        compression=None,
        max_size=None,
        open_timeout=args.connect_timeout,
        close_timeout=args.io_timeout,
        ping_interval=20,
        ping_timeout=20,
    ) as websocket:
        while not should_stop(args, frame_count, deadline):
            now = time.monotonic()
            if now < next_send:
                await asyncio.sleep(min(next_send - now, 0.25))
                continue

            request_size = planned_frame_size(
                args.client_bps,
                args.frame_rate,
                args.min_frame_size,
                args.max_frame_size,
                args.jitter,
                rng,
                REQUEST_HEADER.size,
            )
            request_payload = build_request_payload(frame_count, request_size, args.seed)
            await websocket.send(request_payload)
            response_payload = await asyncio.wait_for(websocket.recv(), timeout=args.io_timeout)
            if response_payload != request_payload:
                raise RuntimeError("invalid echo payload")
            parse_request_payload(response_payload)
            frame_count += 1
            if not ready_emitted:
                touch_ready_file(args.ready_file)
                print("READY client", flush=True)
                ready_emitted = True
            next_send += interval

    return frame_count, ready_emitted, True


async def run_client_async(args):
    deadline = time.monotonic() + args.duration if args.duration is not None else None
    frame_count = 0
    ready_emitted = False
    backoff = args.reconnect_delay

    while True:
        if should_stop(args, frame_count, deadline):
            print(f"DONE frames={frame_count}", flush=True)
            return 0
        try:
            frame_count, ready_emitted, completed = await run_client_connection(
                args, frame_count, ready_emitted, deadline
            )
            if completed and should_stop(args, frame_count, deadline):
                print(f"DONE frames={frame_count}", flush=True)
                return 0
            backoff = args.reconnect_delay
        except (asyncio.TimeoutError, ConnectionClosed, OSError, RuntimeError, ssl.SSLError) as error:
            log(f"event=client_connection_failed frames={frame_count} error={error.__class__.__name__}")
            if not args.reconnect:
                return 1
        await asyncio.sleep(backoff)
        backoff = min(args.reconnect_delay_max, backoff * 2.0)


def run_client(args):
    if args.frame_rate <= 0:
        raise ValueError("--frame-rate must be positive")
    if args.client_bps <= 0:
        raise ValueError("--client-bps must be positive")
    if args.min_frame_size <= 0 or args.max_frame_size <= 0:
        raise ValueError("--min-frame-size and --max-frame-size must be positive")
    if args.min_frame_size > args.max_frame_size:
        raise ValueError("--min-frame-size must not exceed --max-frame-size")
    if args.jitter < 0.0 or args.jitter > 1.0:
        raise ValueError("--jitter must be in 0..1")
    if args.frames is not None and args.frames < 0:
        raise ValueError("--frames must not be negative")
    if args.duration is not None and args.duration < 0.0:
        raise ValueError("--duration must not be negative")
    if args.reconnect_delay <= 0.0 or args.reconnect_delay_max <= 0.0:
        raise ValueError("--reconnect-delay values must be positive")
    return asyncio.run(run_client_async(args))


def add_common_frame_options(parser):
    parser.add_argument("--client-bps", type=int, default=4096)
    parser.add_argument("--frame-rate", type=float, default=4.0)
    parser.add_argument("--min-frame-size", type=int, default=64)
    parser.add_argument("--max-frame-size", type=int, default=16384)
    parser.add_argument("--jitter", type=float, default=0.0)
    parser.add_argument("--seed", type=int, default=1)


def main():
    parser = argparse.ArgumentParser(description="Debug HTTPS/WSS carrier generator for FPS")
    subparsers = parser.add_subparsers(dest="command", required=True)

    origin = subparsers.add_parser("origin", help="serve a TLS HTTPS/WSS echo origin")
    origin.add_argument("--listen", required=True, help="HOST:PORT")
    origin.add_argument("--cert", required=True)
    origin.add_argument("--key", required=True)
    origin.add_argument("--generate-self-signed", action="store_true")
    origin.add_argument("--path", default="/fps-carrier")
    origin.add_argument("--max-frame-size", type=int, default=16384)
    origin.set_defaults(func=run_origin)

    client = subparsers.add_parser("client", help="maintain a TLS WebSocket carrier client")
    client.add_argument("--connect", required=True, help="HOST:PORT")
    client.add_argument("--path", default="/fps-carrier")
    client.add_argument("--server-name", default="localhost")
    client.add_argument("--ca-file")
    client.add_argument("--frames", type=int)
    client.add_argument("--duration", type=float)
    client.add_argument("--ready-file")
    client.add_argument("--reconnect", dest="reconnect", action="store_true", default=True)
    client.add_argument("--no-reconnect", dest="reconnect", action="store_false")
    client.add_argument("--reconnect-delay", type=float, default=0.2)
    client.add_argument("--reconnect-delay-max", type=float, default=5.0)
    client.add_argument("--connect-timeout", type=float, default=5.0)
    client.add_argument("--io-timeout", type=float, default=5.0)
    add_common_frame_options(client)
    client.set_defaults(func=run_client)

    args = parser.parse_args()
    try:
        return args.func(args)
    except KeyboardInterrupt:
        return 130
    except Exception as error:
        log(f"error={error}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
