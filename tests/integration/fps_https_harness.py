import http.server
import base64
import select
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time
from pathlib import Path

ROUND_PATHS = ["/round/0", "/round/1", "/round/2"]
LARGE_PATH_PREFIX = "/large/"
ZERO_RTT_CLIENT_PRIVATE_HEX = (
    "292a2b2c2d2e2f303132333435363738"
    "393a3b3c3d3e3f404142434445464748"
)
ZERO_RTT_CLIENT_PUBLIC_HEX = (
    "46b1880a0dca97ae1618d3d8c04cc51b"
    "10fd08c1cd8839c1bbf4bd7fe014005d"
)
ZERO_RTT_SERVER_PRIVATE_HEX = (
    "65666768696a6b6c6d6e6f7071727374"
    "75767778797a7b7c7d7e7f8081828384"
)
ZERO_RTT_SERVER_PUBLIC_HEX = (
    "5714769d116bf76436ae74bc793d2c30"
    "ad1903c59ac5273805c7e2698b410c36"
)
ZERO_RTT_CLIENT_UUID = "123e4567-e89b-42d3-a456-426614174000"
ZERO_RTT_DECOY_CLIENT_UUIDS = [
    "223e4567-e89b-42d3-a456-426614174000",
    "323e4567-e89b-42d3-a456-426614174000",
]
ZERO_RTT_SERVER_PRIVATE_BASE64 = base64.b64encode(
    bytes.fromhex(ZERO_RTT_SERVER_PRIVATE_HEX)
).decode("ascii")
ZERO_RTT_SERVER_PUBLIC_BASE64 = base64.b64encode(
    bytes.fromhex(ZERO_RTT_SERVER_PUBLIC_HEX)
).decode("ascii")


def response_body(path):
    if path.startswith(LARGE_PATH_PREFIX):
        size = int(path[len(LARGE_PATH_PREFIX) :])
        return bytes((0x41 + (index % 23)) for index in range(size))
    return f"fps-response:{path}\n".encode("ascii")


class RecordingHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        self.server.request_paths.append(self.path)
        body = response_body(self.path)
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        return


class RecordingHTTPSServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, server_address, handler_class):
        super().__init__(server_address, handler_class)
        self.request_paths = []


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def generate_cert(tmpdir):
    openssl = shutil.which("openssl")
    if openssl is None:
        raise RuntimeError("openssl executable is required for this integration test")

    cert = Path(tmpdir) / "cert.pem"
    key = Path(tmpdir) / "key.pem"
    subprocess.run(
        [
            openssl,
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            str(key),
            "-out",
            str(cert),
            "-subj",
            "/CN=localhost",
            "-days",
            "1",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=True,
    )
    return cert, key


def start_https_origin(cert, key, port):
    server = RecordingHTTPSServer(("127.0.0.1", port), RecordingHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def stop_origin(server):
    server.shutdown()
    server.server_close()


def wait_for_tcp(host, port, process, timeout=5.0):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        assert_process_alive(process, "relay")
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return
        except OSError as error:
            last_error = error
            time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {host}:{port}: {last_error}")


def wait_for_output_line(process, name, expected="READY", timeout=5.0):
    deadline = time.monotonic() + timeout
    collected = []
    while time.monotonic() < deadline:
        assert_process_alive(process, name)
        ready, _, _ = select.select([process.stdout], [], [], 0.1)
        if not ready:
            continue
        line = process.stdout.readline()
        if not line:
            continue
        collected.append(line.rstrip())
        if expected in line:
            return
    raise RuntimeError(f"timed out waiting for {name} {expected}: {collected!r}")


def assert_process_alive(process, name):
    if process.poll() is None:
        return
    output = process.stdout.read() if process.stdout is not None else ""
    raise RuntimeError(f"{name} exited early with {process.returncode}: {output}")


def terminate(process):
    if process is None:
        return
    process.terminate()
    try:
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)


def stop_and_read(process):
    if process is None:
        return ""
    terminate(process)
    if process.stdout is None:
        return ""
    return process.stdout.read()


def start_process(args):
    return subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def carrier_command(carrier):
    if carrier.endswith(".py"):
        return [sys.executable, carrier]
    return [carrier]


def run_carrier_client(carrier, port, path, frames=3, seed=1):
    completed = subprocess.run(
        [
            *carrier_command(carrier),
            "client",
            "--connect",
            f"127.0.0.1:{port}",
            "--path",
            path,
            "--frames",
            str(frames),
            "--client-bps",
            "2048",
            "--frame-rate",
            "8",
            "--seed",
            str(seed),
            "--no-reconnect",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=10.0,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"carrier client failed:\n{completed.stdout}")
    if f"DONE frames={frames}" not in completed.stdout:
        raise RuntimeError(f"unexpected carrier client output:\n{completed.stdout}")


def start_carrier_origin(carrier, cert, key, port, path):
    process = start_process(
        [
            *carrier_command(carrier),
            "origin",
            "--listen",
            f"127.0.0.1:{port}",
            "--cert",
            str(cert),
            "--key",
            str(key),
            "--path",
            path,
        ]
    )
    wait_for_output_line(process, "carrier origin")
    return process


def read_http_response(fileobj):
    status = fileobj.readline()
    if not status:
        raise RuntimeError("connection closed before HTTP status")
    if not status.startswith(b"HTTP/1.1 200"):
        raise RuntimeError(status.decode("latin1", "replace").rstrip())

    headers = {}
    while True:
        line = fileobj.readline()
        if not line:
            raise RuntimeError("connection closed before HTTP headers completed")
        if line == b"\r\n":
            break
        name, _, value = line.decode("latin1", "replace").partition(":")
        headers[name.lower()] = value.strip()

    length = int(headers["content-length"])
    return fileobj.read(length)


def https_get_roundtrips(port, paths=ROUND_PATHS):
    context = ssl._create_unverified_context()
    bodies = []
    with socket.create_connection(("127.0.0.1", port), timeout=5.0) as raw:
        raw.settimeout(5.0)
        with context.wrap_socket(raw, server_hostname="localhost") as sock:
            sock.settimeout(5.0)
            fileobj = sock.makefile("rb")
            for path in paths:
                request = (
                    f"GET {path} HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                ).encode("ascii")
                sock.sendall(request)
                bodies.append(read_http_response(fileobj))
    return bodies


def assert_roundtrip_bodies(bodies, paths=ROUND_PATHS):
    expected = [response_body(path) for path in paths]
    if bodies != expected:
        raise RuntimeError(f"unexpected response bodies: {bodies!r}")


def assert_origin_paths(origin, paths=ROUND_PATHS):
    if origin.request_paths != paths:
        raise RuntimeError(f"unexpected origin request paths: {origin.request_paths!r}")


def assert_log_contains(logs, needle):
    if needle not in logs:
        raise RuntimeError(f"missing log marker {needle!r} in:\n{logs}")


def assert_log_omits(logs, needle):
    if needle in logs:
        raise RuntimeError(f"forbidden log marker {needle!r} appeared in:\n{logs}")


def prepare_zero_rtt_fixture_dir(tmpdir):
    Path(tmpdir).mkdir(parents=True, exist_ok=True)


def write_zero_rtt_relay_config(
    path,
    listen_port,
    target_name,
    target_port,
    role,
    decoy_allowed_clients=0,
    read_buffer_size=4096,
    client_uuid=ZERO_RTT_CLIENT_UUID,
    status_socket=None,
):
    if role not in {"client", "server"}:
        raise ValueError(f"unsupported zero-rtt role: {role}")
    if decoy_allowed_clients < 0 or decoy_allowed_clients > len(ZERO_RTT_DECOY_CLIENT_UUIDS):
        raise ValueError(f"unsupported decoy client count: {decoy_allowed_clients}")

    if role == "client":
        key_config = """
      "client_uuid": "%s",
      "server_public_key_base64": "%s",
""" % (client_uuid, ZERO_RTT_SERVER_PUBLIC_BASE64)
    else:
        allowed = ZERO_RTT_DECOY_CLIENT_UUIDS[:decoy_allowed_clients] + [client_uuid]
        allowed_json = ", ".join(f'"{uuid}"' for uuid in allowed)
        key_config = """
      "server_private_key_base64": "%s",
      "server_public_key_base64": "%s",
      "allowed_client_uuids": [%s],
""" % (ZERO_RTT_SERVER_PRIVATE_BASE64, ZERO_RTT_SERVER_PUBLIC_BASE64, allowed_json)

    ops_config = ""
    if status_socket is not None:
        ops_config = """,
  "ops": {
    "status_socket": "%s"
  }
""" % status_socket

    path.write_text(
        """
{
  "network": {
    "listen": "127.0.0.1:%d",
    "%s": "127.0.0.1:%d",
    "read_buffer_size": %d
  },
  "security": {
    "zero_rtt": {
      "enabled": true,
      "profile_id": "integration-origin-v3",
%s      "version": 3,
      "capabilities": 1,
      "max_padding_size": 64,
      "min_records_before_trial": 1,
      "upgrade_direction": "client_to_server"
    }
  },
  "codec": {
    "max_frame_payload": 1024,
    "max_frame_padding": 64,
    "allow_fragmentation": true
  }
%s
}
"""
        % (listen_port,
           target_name,
           target_port,
           read_buffer_size,
           key_config,
           ops_config),
        encoding="utf-8",
    )
