#!/usr/bin/env bash
set -euo pipefail

log() {
  printf 'fps-dante-entrypoint: %s\n' "$*" >&2
}

die() {
  log "error: $*"
  exit 2
}

usage() {
  cat <<'EOF'
Usage: fps-dante-entrypoint.sh [run]

Environment:
  FPS_SOCKS_LISTEN_ADDRESS=10.66.0.1
  FPS_SOCKS_PORT=1080
  FPS_SOCKS_ALLOWED_CIDR=10.66.0.0/24
  FPS_SOCKS_EXTERNAL_INTERFACE=eth0
  FPS_SOCKS_WAIT_ATTEMPTS=150
  FPS_SOCKS_WAIT_INTERVAL=0.2
EOF
}

wait_for_listen_address() {
  local address="$1"
  local attempts="${FPS_SOCKS_WAIT_ATTEMPTS:-150}"
  local sleep_seconds="${FPS_SOCKS_WAIT_INTERVAL:-0.2}"

  [[ "$attempts" =~ ^[0-9]+$ ]] || die "FPS_SOCKS_WAIT_ATTEMPTS must be an integer"
  command -v ip >/dev/null 2>&1 || die "iproute2/ip command is required"

  local attempt=0
  while (( attempt < attempts )); do
    if ip -o addr show | grep -Fq " ${address}/"; then
      return 0
    fi
    sleep "$sleep_seconds"
    attempt=$((attempt + 1))
  done

  die "timed out waiting for SOCKS listen address ${address}"
}

render_danted_config() {
  local path="$1"
  local listen_address="$2"
  local port="$3"
  local allowed_cidr="$4"
  local external_interface="$5"

  cat >"$path" <<EOF
logoutput: stderr
internal: ${listen_address} port = ${port}
external: ${external_interface}
socksmethod: none
clientmethod: none
user.privileged: root
user.unprivileged: nobody

client pass {
        from: ${allowed_cidr} to: 0.0.0.0/0
        log: connect disconnect error
}

client block {
        from: 0.0.0.0/0 to: 0.0.0.0/0
        log: connect error
}

socks pass {
        from: ${allowed_cidr} to: 0.0.0.0/0
        command: connect
        log: connect disconnect error
}

socks block {
        from: 0.0.0.0/0 to: 0.0.0.0/0
        log: connect error
}
EOF
}

run_socks() {
  local listen_address="${FPS_SOCKS_LISTEN_ADDRESS:-10.66.0.1}"
  local port="${FPS_SOCKS_PORT:-1080}"
  local allowed_cidr="${FPS_SOCKS_ALLOWED_CIDR:-10.66.0.0/24}"
  local external_interface="${FPS_SOCKS_EXTERNAL_INTERFACE:-eth0}"
  local config="${FPS_SOCKS_CONFIG:-/tmp/fps-danted.conf}"

  [[ "$port" =~ ^[0-9]+$ ]] || die "FPS_SOCKS_PORT must be an integer"
  command -v /usr/sbin/danted >/dev/null 2>&1 || die "danted is required"

  wait_for_listen_address "$listen_address"
  render_danted_config "$config" "$listen_address" "$port" "$allowed_cidr" "$external_interface"
  log "starting Dante SOCKS5 listen=${listen_address}:${port} allowed=${allowed_cidr}"
  exec /usr/sbin/danted -f "$config"
}

if [[ $# -gt 0 && "$1" != "run" ]]; then
  if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    exit 0
  fi
  exec "$@"
fi

run_socks
