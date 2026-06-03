#!/usr/bin/env python3

import json
import os
import shlex
import subprocess
import time
from pathlib import Path
from posixpath import normpath


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def run(args, *, cwd=None, check=True, input_text=None):
    completed = subprocess.run(
        args,
        cwd=cwd,
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if check and completed.returncode != 0:
        command = " ".join(str(arg) for arg in args)
        raise RuntimeError(f"command failed ({completed.returncode}): {command}\n{completed.stdout}")
    return completed


def docker_base(force_sudo: bool = False) -> list[str]:
    if force_sudo or os.environ.get("FPS_DOCKER_SUDO") == "1":
        return ["sudo", "-n", "docker"]
    probe = run(["docker", "info"], check=False)
    if probe.returncode == 0:
        return ["docker"]
    sudo_probe = run(["sudo", "-n", "docker", "info"], check=False)
    if sudo_probe.returncode == 0:
        return ["sudo", "-n", "docker"]
    raise RuntimeError(
        "docker daemon is unavailable; set FPS_DOCKER_SUDO=1 if this host requires sudo"
    )


def docker(base: list[str], args: list[str], *, cwd=None, check=True, input_text=None):
    return run([*base, *args], cwd=cwd, check=check, input_text=input_text)


def shell_join(args: list[str]) -> str:
    return " ".join(shlex.quote(str(arg)) for arg in args)


def ssh(remote: str, command: str, *, check=True):
    return run(["ssh", remote, command], check=check)


def ensure_safe_remote_work_dir(remote_dir: str):
    normalized = normpath(remote_dir.strip())
    forbidden = {"", ".", "/", "/tmp", "/var", "/var/tmp", "/home", "/root"}
    if normalized in forbidden or not normalized.startswith("/"):
        raise ValueError(f"unsafe remote work directory: {remote_dir!r}")
    if normalized.count("/") < 2:
        raise ValueError(f"remote work directory is too broad: {remote_dir!r}")
    return normalized


def remote_compose(
    remote: str,
    remote_dir: str,
    project: str,
    args: list[str],
    *,
    compose_file: str = "compose.yml",
    remote_docker: str = "docker",
    check=True,
):
    remote_dir = ensure_safe_remote_work_dir(remote_dir)
    command = (
        f"cd {shlex.quote(remote_dir)} && "
        f"{remote_docker} compose -p {shlex.quote(project)} "
        f"-f {shlex.quote(compose_file)} {shell_join(args)}"
    )
    return ssh(remote, command, check=check)


def remote_compose_exec(
    remote: str,
    remote_dir: str,
    project: str,
    service: str,
    command: list[str],
    *,
    compose_file: str = "compose.yml",
    remote_docker: str = "docker",
    check=True,
):
    return remote_compose(
        remote,
        remote_dir,
        project,
        ["exec", "-T", service, *command],
        compose_file=compose_file,
        remote_docker=remote_docker,
        check=check,
    )


def copy_tree_to_remote(local_dir: Path, remote: str, remote_dir: str):
    local_dir = local_dir.resolve()
    remote_dir = ensure_safe_remote_work_dir(remote_dir)
    mkdir = f"rm -rf {shlex.quote(remote_dir)} && mkdir -p {shlex.quote(remote_dir)}"
    ssh(remote, mkdir)
    producer = subprocess.Popen(
        ["tar", "-C", str(local_dir), "-cf", "-", "."],
        stdout=subprocess.PIPE,
    )
    assert producer.stdout is not None
    consumer = subprocess.run(
        ["ssh", remote, f"tar -C {shlex.quote(remote_dir)} -xf -"],
        stdin=producer.stdout,
        text=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    producer.stdout.close()
    producer_rc = producer.wait()
    if producer_rc != 0 or consumer.returncode != 0:
        output = consumer.stdout.decode("utf-8", "replace") if consumer.stdout else ""
        raise RuntimeError(
            f"failed to copy {local_dir} to {remote}:{remote_dir}: "
            f"tar={producer_rc} ssh-tar={consumer.returncode}\n{output}"
        )


def transfer_docker_image(base: list[str], remote: str, image: str, *, remote_docker: str = "docker"):
    producer = subprocess.Popen([*base, "save", image], stdout=subprocess.PIPE)
    assert producer.stdout is not None
    consumer = subprocess.run(
        ["ssh", remote, f"{remote_docker} load"],
        stdin=producer.stdout,
        text=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    producer.stdout.close()
    producer_rc = producer.wait()
    if producer_rc != 0 or consumer.returncode != 0:
        output = consumer.stdout.decode("utf-8", "replace") if consumer.stdout else ""
        raise RuntimeError(
            f"failed to transfer Docker image {image!r} to {remote}: "
            f"docker-save={producer_rc} remote-load={consumer.returncode}\n{output}"
        )
    return consumer.stdout.decode("utf-8", "replace") if consumer.stdout else ""


def compose(base: list[str], compose_file: Path, project: str, args: list[str], *, check=True):
    return docker(
        base,
        ["compose", "-p", project, "-f", str(compose_file), *args],
        check=check,
    )


def compose_exec(
    base: list[str],
    compose_file: Path,
    project: str,
    service: str,
    command: list[str],
    *,
    check=True,
):
    return compose(
        base,
        compose_file,
        project,
        ["exec", "-T", service, *command],
        check=check,
    )


def write_json(path: Path, data: dict):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def generate_server_keypair(base: list[str], image: str) -> dict[str, str]:
    completed = docker(
        base,
        [
            "run",
            "--rm",
            image,
            "fps_server",
            "--generate-server-keypair",
        ],
    )
    values = {}
    for line in completed.stdout.splitlines():
        name, _, value = line.partition("=")
        if name in {"server_private_key_base64", "server_public_key_base64"} and value:
            values[name] = value
    missing = {"server_private_key_base64", "server_public_key_base64"} - values.keys()
    if missing:
        raise RuntimeError(f"server key helper output missing {sorted(missing)}:\n{completed.stdout}")
    return values


def wait_for_services(
    base: list[str],
    compose_file: Path,
    project: str,
    services: list[str],
    timeout: float,
):
    deadline = time.monotonic() + timeout
    expected = set(services)
    last = ""
    while time.monotonic() < deadline:
        result = compose(base, compose_file, project, ["ps", "--services", "--status", "running"])
        running = set(result.stdout.split())
        if expected.issubset(running):
            return
        last = result.stdout
        time.sleep(0.5)
    raise RuntimeError(f"services did not all become running: expected={services!r} running={last!r}")


def wait_for_log(
    base: list[str],
    compose_file: Path,
    project: str,
    pattern: str,
    timeout: float,
    services: list[str],
):
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        result = compose(base, compose_file, project, ["logs", "--no-color", *services])
        last = result.stdout
        if pattern in last:
            return last
        time.sleep(0.5)
    raise RuntimeError(f"timed out waiting for log pattern {pattern!r}:\n{last}")


def wait_for_log_count(
    base: list[str],
    compose_file: Path,
    project: str,
    pattern: str,
    count: int,
    timeout: float,
    services: list[str],
) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        result = compose(base, compose_file, project, ["logs", "--no-color", *services])
        last = result.stdout
        if last.count(pattern) >= count:
            return last
        time.sleep(0.5)
    raise RuntimeError(f"timed out waiting for {count} log entries {pattern!r}:\n{last}")


def parse_iperf_udp_summary(output: str) -> dict:
    data = json.loads(output)
    end = data.get("end", {})
    for key in ("sum", "sum_received", "sum_sent"):
        candidate = end.get(key)
        if isinstance(candidate, dict) and "bits_per_second" in candidate:
            packets = candidate.get("packets", 0) or 0
            lost_packets = candidate.get("lost_packets", 0) or 0
            lost_percent = candidate.get("lost_percent")
            if lost_percent is None and packets:
                lost_percent = (lost_packets / packets) * 100.0
            return {
                "bits_per_second": candidate.get("bits_per_second", 0.0),
                "mbits_per_second": candidate.get("bits_per_second", 0.0) / 1_000_000.0,
                "jitter_ms": candidate.get("jitter_ms", 0.0),
                "lost_packets": lost_packets,
                "packets": packets,
                "lost_percent": lost_percent if lost_percent is not None else 0.0,
                "seconds": candidate.get("seconds", 0.0),
                "bytes": candidate.get("bytes", 0),
            }
    raise RuntimeError(f"cannot find UDP summary in iperf3 JSON:\n{output}")


def extract_session_stats(logs: str) -> list[str]:
    return [line for line in logs.splitlines() if "event=session_stats" in line]


def stats_have_tun_traffic(lines: list[str]) -> bool:
    for line in lines:
        if " stats=" in line:
            raw_stats = line.split(" stats=", 1)[1].strip()
            try:
                stats, _ = json.JSONDecoder().raw_decode(raw_stats)
            except json.JSONDecodeError:
                stats = {}
            for direction in ("client_to_server", "server_to_client"):
                direction_stats = stats.get(direction, {})
                if any(int(direction_stats.get(name, 0)) > 0 for name in ("datagram_frames_in", "datagram_frames_out")):
                    return True
    return False


def ignore_private_artifacts(_directory, names):
    return {name for name in names if name.endswith(".key")}
