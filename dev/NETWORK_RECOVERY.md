# Network / Capture Recovery Notes

Date: 2026-05-11

Incident: an early pcap-capture run left `tcpdump` processes stuck under the
system AppArmor `tcpdump` profile. The VM was rebooted by the operator. After
reboot, FPS network namespaces and capture processes were gone.

## Quick Check After Reboot

```sh
ip netns list | rg 'fps[cs]|fpsprobe|fpstest' || true
ps -ef | rg 'tcpdump|capture_tls_wire|pcap_tls_shape|tun_zero_rtt_loopback' || true
```

Expected result: no FPS network namespaces and no leftover capture/test
processes.

## If Capture Processes Hang Again

If process cleanup is blocked by AppArmor again, rebooting the VM is the most
reliable recovery path. If shell access still works, try:

```sh
sudo -n aa-complain tcpdump || true
sudo -n aa-exec -p unconfined -- pkill -KILL -f 'tcpdump.*fps' || true
sudo -n pkill -KILL -f 'capture_tls_wire|pcap_tls_shape|tun_zero_rtt_loopback' || true
sudo -n aa-enforce tcpdump || true
```

Then remove leftover isolated namespaces:

```sh
for ns in $(ip netns list | awk '/fps[cs]|fpsprobe|fpstest/ {print $1}'); do
  sudo -n ip netns del "$ns" || true
done
```

## Current Mitigation

- `tools/capture_tls_wire.sh` runs `tcpdump` through `aa-exec -p unconfined`
  when available.
- Capture uses `--immediate-mode` and `-U` so short integration runs flush packet
  data before shutdown.
- TUN pcap capture starts after readiness probes, so saved pcaps contain the FPS
  carrier flow rather than only setup probes.
