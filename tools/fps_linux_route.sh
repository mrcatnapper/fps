#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage:
  tools/fps_linux_route.sh plan|apply|cleanup --tun DEV [options]

Configures the Linux host-side routing/DNS pieces around an FPS TUN device.
The script never starts fps_client/fps_server and never edits firewall/NAT rules.

Actions:
  plan       Print commands only.
  apply      Execute commands; requires root unless --dry-run is passed.
  cleanup    Delete/revert the commands represented by the same options.

Common options:
  --tun DEV                 TUN interface name, e.g. fpsc0.
  --tun-address CIDR        Address to place on the TUN interface.
  --mtu MTU                 MTU to set on the TUN interface.
  --route CIDR              Split-tunnel route to send through DEV; repeatable.
  --dns ADDRESS             DNS server to attach to DEV via resolvectl; repeatable.
  --dns-domain DOMAIN       resolvectl routing/search domain; repeatable.
  --no-dns                  Do not emit resolvectl commands.
  --down                    On cleanup, also set DEV down.
  --dry-run                 With apply/cleanup, print commands without executing.

Full-tunnel / policy-routing options:
  --full-tunnel             Put default route in --table via DEV.
  --table TABLE             Routing table for --full-tunnel, default 100.
  --priority PRIORITY       ip rule priority, default 10000.
  --fwmark MARK             Route packets with this fwmark through --table.
  --from CIDR               Route packets from this source CIDR through --table.
  --bypass CIDR,VIA,DEV     In --table, route carrier/control CIDR via underlay.
                            Repeatable. Example: 203.0.113.10/32,192.0.2.1,eth0

Notes:
  Full tunnel requires either --fwmark or --from so that operators choose an
  explicit policy selector and do not accidentally hijack the whole host.
EOF
}

die() {
  echo "Error: $*" >&2
  exit 2
}

print_command() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
}

run_cmd() {
  print_command "$@"
  if [[ "$execute" == true ]]; then
    "$@"
  fi
}

run_optional_cmd() {
  print_command "$@"
  if [[ "$execute" == true ]]; then
    "$@" 2>/dev/null || true
  fi
}

action="${1:-}"
if [[ -z "$action" || "$action" == "-h" || "$action" == "--help" ]]; then
  usage
  exit 0
fi
case "$action" in
  plan|apply|cleanup)
    shift
    ;;
  *)
    usage
    exit 2
    ;;
esac

tun=""
tun_address=""
mtu=""
routes=()
dns_servers=()
dns_domains=()
manage_dns=true
down=false
dry_run=false
full_tunnel=false
table="100"
priority="10000"
fwmark=""
from_selector=""
bypass_routes=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tun)
      [[ $# -ge 2 ]] || die "missing value for --tun"
      tun="$2"
      shift 2
      ;;
    --tun-address)
      [[ $# -ge 2 ]] || die "missing value for --tun-address"
      tun_address="$2"
      shift 2
      ;;
    --mtu)
      [[ $# -ge 2 ]] || die "missing value for --mtu"
      mtu="$2"
      shift 2
      ;;
    --route)
      [[ $# -ge 2 ]] || die "missing value for --route"
      routes+=("$2")
      shift 2
      ;;
    --dns)
      [[ $# -ge 2 ]] || die "missing value for --dns"
      dns_servers+=("$2")
      shift 2
      ;;
    --dns-domain)
      [[ $# -ge 2 ]] || die "missing value for --dns-domain"
      dns_domains+=("$2")
      shift 2
      ;;
    --no-dns)
      manage_dns=false
      shift
      ;;
    --down)
      down=true
      shift
      ;;
    --dry-run)
      dry_run=true
      shift
      ;;
    --full-tunnel)
      full_tunnel=true
      shift
      ;;
    --table)
      [[ $# -ge 2 ]] || die "missing value for --table"
      table="$2"
      shift 2
      ;;
    --priority)
      [[ $# -ge 2 ]] || die "missing value for --priority"
      priority="$2"
      shift 2
      ;;
    --fwmark)
      [[ $# -ge 2 ]] || die "missing value for --fwmark"
      fwmark="$2"
      shift 2
      ;;
    --from)
      [[ $# -ge 2 ]] || die "missing value for --from"
      from_selector="$2"
      shift 2
      ;;
    --bypass)
      [[ $# -ge 2 ]] || die "missing value for --bypass"
      bypass_routes+=("$2")
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

[[ -n "$tun" ]] || die "--tun DEV is required"
if [[ "$full_tunnel" == true && -z "$fwmark" && -z "$from_selector" ]]; then
  die "--full-tunnel requires --fwmark MARK or --from CIDR"
fi

execute=false
if [[ "$action" == "apply" || "$action" == "cleanup" ]]; then
  execute=true
fi
if [[ "$dry_run" == true ]]; then
  execute=false
fi
if [[ "$execute" == true && "${EUID:-$(id -u)}" -ne 0 ]]; then
  die "$action requires root; rerun with sudo or use plan/--dry-run"
fi
if [[ "$execute" == true ]]; then
  command -v ip >/dev/null 2>&1 || die "ip command is required"
  if [[ "$manage_dns" == true &&
        (${#dns_servers[@]} -gt 0 || ${#dns_domains[@]} -gt 0 || "$action" == "cleanup") ]]; then
    command -v resolvectl >/dev/null 2>&1 || die "resolvectl is required for DNS operations"
  fi
fi

echo "# fps_linux_route action=$action tun=$tun execute=$execute"

if [[ "$action" == "cleanup" ]]; then
  if [[ "$manage_dns" == true ]]; then
    run_optional_cmd resolvectl revert "$tun"
  fi
  for route in "${routes[@]}"; do
    run_optional_cmd ip route del "$route" dev "$tun"
  done
  if [[ "$full_tunnel" == true ]]; then
    if [[ -n "$fwmark" ]]; then
      run_optional_cmd ip rule del priority "$priority" fwmark "$fwmark" table "$table"
    fi
    if [[ -n "$from_selector" ]]; then
      run_optional_cmd ip rule del priority "$priority" from "$from_selector" table "$table"
    fi
    for bypass in "${bypass_routes[@]}"; do
      IFS=, read -r bypass_cidr bypass_via bypass_dev <<<"$bypass"
      [[ -n "${bypass_cidr:-}" && -n "${bypass_via:-}" && -n "${bypass_dev:-}" ]] ||
        die "invalid --bypass value: $bypass"
      run_optional_cmd ip route del "$bypass_cidr" via "$bypass_via" dev "$bypass_dev" table "$table"
    done
    run_optional_cmd ip route del default dev "$tun" table "$table"
    run_optional_cmd ip route flush cache
  fi
  if [[ -n "$tun_address" ]]; then
    run_optional_cmd ip addr del "$tun_address" dev "$tun"
  fi
  if [[ "$down" == true ]]; then
    run_optional_cmd ip link set dev "$tun" down
  fi
  exit 0
fi

if [[ -n "$tun_address" ]]; then
  run_cmd ip addr replace "$tun_address" dev "$tun"
fi
if [[ -n "$mtu" ]]; then
  run_cmd ip link set dev "$tun" mtu "$mtu"
fi
run_cmd ip link set dev "$tun" up

if [[ "$full_tunnel" == true ]]; then
  for bypass in "${bypass_routes[@]}"; do
    IFS=, read -r bypass_cidr bypass_via bypass_dev <<<"$bypass"
    [[ -n "${bypass_cidr:-}" && -n "${bypass_via:-}" && -n "${bypass_dev:-}" ]] ||
      die "invalid --bypass value: $bypass"
    run_cmd ip route replace "$bypass_cidr" via "$bypass_via" dev "$bypass_dev" table "$table"
  done
  run_cmd ip route replace default dev "$tun" table "$table"
  if [[ -n "$fwmark" ]]; then
    run_optional_cmd ip rule del priority "$priority" fwmark "$fwmark" table "$table"
    run_cmd ip rule add priority "$priority" fwmark "$fwmark" table "$table"
  fi
  if [[ -n "$from_selector" ]]; then
    run_optional_cmd ip rule del priority "$priority" from "$from_selector" table "$table"
    run_cmd ip rule add priority "$priority" from "$from_selector" table "$table"
  fi
  run_cmd ip route flush cache
else
  for route in "${routes[@]}"; do
    run_cmd ip route replace "$route" dev "$tun"
  done
fi

if [[ "$manage_dns" == true ]]; then
  if [[ ${#dns_servers[@]} -gt 0 ]]; then
    run_cmd resolvectl dns "$tun" "${dns_servers[@]}"
  fi
  if [[ ${#dns_domains[@]} -gt 0 ]]; then
    run_cmd resolvectl domain "$tun" "${dns_domains[@]}"
  fi
fi
