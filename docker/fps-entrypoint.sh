#!/usr/bin/env bash
set -euo pipefail

log() {
  printf 'fps-entrypoint: %s\n' "$*" >&2
}

die() {
  log "error: $*"
  exit 2
}

split_csv() {
  local value="$1"
  local -n out_ref="$2"
  out_ref=()
  if [[ -z "$value" ]]; then
    return 0
  fi
  local old_ifs="$IFS"
  IFS=,
  read -r -a out_ref <<<"$value"
  IFS="$old_ifs"
}

configure_tun_once() {
  local tun_name="${FPS_TUN_NAME:-}"
  [[ -n "$tun_name" ]] || die "FPS_CONFIGURE_TUN=1 requires FPS_TUN_NAME"

  command -v ip >/dev/null 2>&1 || die "iproute2/ip command is required"

  if [[ -n "${FPS_TUN_ADDRESS:-}" ]]; then
    ip addr replace "$FPS_TUN_ADDRESS" dev "$tun_name"
  fi
  if [[ -n "${FPS_TUN_MTU:-}" ]]; then
    ip link set dev "$tun_name" mtu "$FPS_TUN_MTU"
  fi
  ip link set dev "$tun_name" up

  local routes=()
  split_csv "${FPS_TUN_ROUTES:-}" routes
  local route
  for route in "${routes[@]}"; do
    [[ -n "$route" ]] || continue
    ip route replace "$route" dev "$tun_name"
  done
}

wait_and_configure_tun() {
  local fps_pid="$1"
  local tun_name="${FPS_TUN_NAME:-}"
  local attempts="${FPS_TUN_WAIT_ATTEMPTS:-150}"
  local sleep_seconds="${FPS_TUN_WAIT_INTERVAL:-0.2}"

  [[ "$attempts" =~ ^[0-9]+$ ]] || die "FPS_TUN_WAIT_ATTEMPTS must be an integer"
  [[ -n "$tun_name" ]] || die "FPS_CONFIGURE_TUN=1 requires FPS_TUN_NAME"

  local attempt=0
  while (( attempt < attempts )); do
    if ! kill -0 "$fps_pid" 2>/dev/null; then
      log "fps process exited before TUN interface appeared"
      return 1
    fi
    if ip link show dev "$tun_name" >/dev/null 2>&1; then
      log "configuring TUN interface $tun_name"
      configure_tun_once
      return 0
    fi
    sleep "$sleep_seconds"
    attempt=$((attempt + 1))
  done

  log "timed out waiting for TUN interface $tun_name"
  return 1
}

role_context() {
  local role="${FPS_ROLE:-server}"
  local config="${FPS_CONFIG:-}"

  case "$role" in
    client|server)
      ;;
    *)
      die "FPS_ROLE must be client or server"
      ;;
  esac

  if [[ -z "$config" ]]; then
    config="/etc/fps/${role}.json"
  fi

  local binary="/usr/local/bin/fps_${role}"
  [[ -x "$binary" ]] || die "missing executable: $binary"
  [[ -r "$config" ]] || die "cannot read FPS_CONFIG: $config"

  printf '%s\n' "$binary" "$config"
}

role_log_args() {
  if [[ -n "${FPS_LOG_LEVEL:-}" ]]; then
    printf '%s\n' --log-level "$FPS_LOG_LEVEL"
  fi
}

run_role_alias() {
  local alias_name="$1"
  shift

  local context=()
  mapfile -t context < <(role_context)
  local binary="${context[0]}"
  local config="${context[1]}"
  local log_args=()
  mapfile -t log_args < <(role_log_args)

  case "$alias_name" in
    check-config)
      exec "$binary" --check-config --config "$config" "${log_args[@]}" "$@"
      ;;
    status)
      exec "$binary" --status --config "$config" "${log_args[@]}" "$@"
      ;;
    *)
      die "unknown role alias: $alias_name"
      ;;
  esac
}

run_fps() {
  local context=()
  mapfile -t context < <(role_context)
  local binary="${context[0]}"
  local config="${context[1]}"

  local log_args=()
  mapfile -t log_args < <(role_log_args)

  "$binary" --check-config --config "$config" "${log_args[@]}"

  local args=("$binary" --config "$config" "${log_args[@]}")
  if [[ "${FPS_CONFIGURE_TUN:-0}" == "1" ]]; then
    "${args[@]}" &
    local fps_pid=$!
    trap 'kill -TERM "$fps_pid" 2>/dev/null || true' INT TERM
    if ! wait_and_configure_tun "$fps_pid"; then
      kill -TERM "$fps_pid" 2>/dev/null || true
      wait "$fps_pid" 2>/dev/null || true
      return 1
    fi
    wait "$fps_pid"
    return $?
  fi

  exec "${args[@]}"
}

if [[ $# -eq 0 || "$1" == "run" ]]; then
  run_fps
  exit $?
fi

case "$1" in
  check-config|status)
    alias_name="$1"
    shift
    run_role_alias "$alias_name" "$@"
    ;;
  *)
    exec "$@"
    ;;
esac
