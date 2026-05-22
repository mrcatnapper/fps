#!/usr/bin/env bash
set -euo pipefail

compiler="${FPS_COMPILER:-gcc}"
jobs="${FPS_JOBS:-2}"
build_dir="${FPS_CI_BUILD_DIR:-/workspaces/build-ci-${compiler}}"

case "$compiler" in
  gcc)
    cxx=g++
    ;;
  clang)
    cxx=clang++-20
    ;;
  *)
    echo "FPS_COMPILER must be gcc or clang" >&2
    exit 2
    ;;
esac

run() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  "$@"
}

cd /workspaces

run python3 -m py_compile tests/integration/*.py tools/*.py
run python3 -c "import websockets"
run bash -n tools/*.sh docker/*.sh examples/docker/proxy-dante/*.sh

rm -rf "$build_dir"
run cmake -S . -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="$cxx" \
  -DFPS_BUILD_TESTS=ON \
  -DFPS_ENABLE_TUN_TESTS=OFF \
  -DFPS_ENABLE_PCAP_TESTS=OFF
run cmake --build "$build_dir" -j "$jobs"
run ctest --test-dir "$build_dir" --output-on-failure
