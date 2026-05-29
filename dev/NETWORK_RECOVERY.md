# Network And Capture Recovery Runbook

This runbook is for local TUN, namespace and packet-capture experiments. It is
not part of the public operator docs.

## Quick Audit

Run this after interrupted TUN, Docker or pcap experiments:

```sh
ip netns list | rg 'fps[cs]|fpsprobe|fpstest' || true
ps -ef | rg 'tcpdump|capture_tls_wire|pcap_tls_shape|tun_zero_rtt_loopback' || true
docker ps --format '{{.Names}}' | rg '^fps-' || true
```

Expected result after cleanup: no FPS test namespaces, no capture processes and
no unexpected FPS containers.

## Clean Isolated Namespaces

```sh
for ns in $(ip netns list | awk '/fps[cs]|fpsprobe|fpstest/ {print $1}'); do
  sudo -n ip netns del "$ns" || true
done
```

## Stop Stuck Capture Processes

Capture helpers normally stop their own `tcpdump` process. If cleanup is
interrupted:

```sh
sudo -n pkill -INT -f 'tcpdump.*fps' || true
sleep 2
sudo -n pkill -KILL -f 'tcpdump.*fps' || true
sudo -n pkill -KILL -f 'capture_tls_wire|pcap_tls_shape|tun_zero_rtt_loopback' || true
```

If the host AppArmor profile blocks cleanup and `aa-exec` is available:

```sh
sudo -n aa-exec -p unconfined -- pkill -KILL -f 'tcpdump.*fps' || true
```

## Docker Cleanup

For compose-based simulations, prefer each tool's normal teardown. For manual
cleanup:

```sh
docker ps --format '{{.Names}}' | awk '/^fps-/ {print $1}' | xargs -r docker rm -f
docker network ls --format '{{.Name}}' | awk '/^fps-/ {print $1}' | xargs -r docker network rm
```

Use `sudo -n docker ...` when the current host requires privileged Docker
access.

## Current Mitigations

- `tools/capture_tls_wire.sh` runs `tcpdump` through `aa-exec -p unconfined`
  when available.
- Capture uses `--immediate-mode` and `-U` so short runs flush packet data
  before shutdown.
- TUN/root tests use isolated namespaces where possible.
