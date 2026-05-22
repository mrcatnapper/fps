#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: tools/capture_tls_wire.sh --port PORT [--interface IFACE] [--out PCAP] -- COMMAND [ARGS...]

Capture one FPS client<->server TCP link with tcpdump while COMMAND runs.
If tshark is installed, the script also writes a TLS record summary next to the pcap.
EOF
}

iface="lo"
out="fps-wire.pcap"
port=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --interface)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      iface="$2"
      shift 2
      ;;
    --out)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      out="$2"
      shift 2
      ;;
    --port)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      port="$2"
      shift 2
      ;;
    --)
      shift
      break
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      exit 2
      ;;
  esac
done

if [[ -z "$port" || $# -eq 0 ]]; then
  usage
  exit 2
fi

if ! command -v tcpdump >/dev/null 2>&1; then
  echo "tcpdump is required" >&2
  exit 2
fi

mkdir -p "$(dirname "$out")"
rm -f "$out" "$out.tls.txt"

tcpdump_cmd=(tcpdump --immediate-mode -i "$iface" -s 0 -U -w "$out" "tcp port $port")
if command -v aa-exec >/dev/null 2>&1; then
  tcpdump_cmd=(aa-exec -p unconfined -- "${tcpdump_cmd[@]}")
fi

sudo -n "${tcpdump_cmd[@]}" >/dev/null 2>&1 &
tcpdump_pid=$!

cleanup() {
  if kill -0 "$tcpdump_pid" >/dev/null 2>&1; then
    child_pids="$(pgrep -P "$tcpdump_pid" 2>/dev/null || true)"
    for child_pid in $child_pids; do
      sudo -n kill -INT "$child_pid" >/dev/null 2>&1 || true
    done
    sudo -n kill -INT "$tcpdump_pid" >/dev/null 2>&1 || true
    wait "$tcpdump_pid" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

sleep 0.3
set +e
"$@"
status=$?
set -e
cleanup
trap - EXIT

if command -v tshark >/dev/null 2>&1; then
  tshark -r "$out" -d "tcp.port==$port,tls" -Y tls.record \
    -T fields \
    -e frame.number \
    -e tcp.stream \
    -e ip.src \
    -e tcp.srcport \
    -e ip.dst \
    -e tcp.dstport \
    -e tls.record.content_type \
    -e tls.record.length > "$out.tls.txt"
  echo "TLS record summary: $out.tls.txt" >&2
else
  echo "Captured $out. Install tshark to produce $out.tls.txt TLS record summaries." >&2
fi

exit "$status"
