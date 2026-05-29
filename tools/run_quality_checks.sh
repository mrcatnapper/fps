#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: tools/run_quality_checks.sh [--python] [--clang] [--sanitizers] [--valgrind] [--coverage] [--fuzz] [--docker] [--soak-smoke] [--all]

Runs repeatable non-sudo QA checks in separate build directories:
  --python       Python integration/tool syntax checks
  --clang       clang++-20 warning build + local CTest
  --sanitizers  clang++-20 ASan+UBSan build + local CTest
  --valgrind    Debug build + Valgrind unit-test pass
  --coverage    clang++-20 coverage build + local CTest + llvm-cov report
  --fuzz        clang++-20 libFuzzer smoke with ASan+UBSan
  --docker      Opt-in Docker build/smoke and compose config checks
  --soak-smoke  Opt-in short Docker/TUN resilience soak
  --all         Run every non-Docker check (default when no options are given)

Environment:
  FPS_JOBS      build parallelism, default: 2
  CLANG_CXX     clang++ path override
  LLVM_COV      llvm-cov path override
  LLVM_PROFDATA llvm-profdata path override
  FPS_DOCKER_IMAGE Docker image tag for --docker, default: fps:local
  FPS_DOCKERFILE Dockerfile path for --docker, default: Dockerfile
  FPS_DOCKER_COMPILER Docker builder compiler for --docker: gcc or clang, default: gcc
  FPS_DOCKER_BUILDKIT=0 Disable BuildKit for Docker builds; default: enabled
  FPS_DOCKER_SUDO=1 Run docker through sudo -n for --docker
  FPS_DOCKER_SOAK_BUILD=0 Do not build image before --soak-smoke; default: build
  FPS_DOCKER_SOAK_DURATION Docker resilience smoke duration, default: 60
  FPS_DOCKER_SOAK_BANDWIDTH Docker resilience UDP bandwidth, default: 500K
  FPS_DOCKER_SOAK_LENGTH Docker resilience UDP datagram size, default: 512
  FPS_DOCKER_SOAK_STRESS=0 Disable write_queue_full stress in --soak-smoke
  FPS_DOCKER_SOAK_REQUIRE_BACKPRESSURE=1 Require write_queue_full during stress
  FPS_DOCKER_SOAK_WITH_SOCKS=1 Include Dante overlay probe in --soak-smoke
  FPS_COVERAGE_MIN_LINES      minimum total line coverage, default: 70
  FPS_COVERAGE_MIN_FUNCTIONS  minimum total function coverage, default: 80
  FPS_FUZZ_RUNS               libFuzzer -runs value, default: 256
  FPS_FUZZ_SECONDS            libFuzzer -max_total_time value, default unset
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${FPS_JOBS:-2}"

run_python=false
run_clang=false
run_sanitizers=false
run_valgrind=false
run_coverage=false
run_fuzz=false
run_docker=false
run_soak_smoke=false

if [[ $# -eq 0 ]]; then
  run_python=true
  run_clang=true
  run_sanitizers=true
  run_valgrind=true
  run_coverage=true
  run_fuzz=true
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --python)
      run_python=true
      ;;
    --clang)
      run_clang=true
      ;;
    --sanitizers)
      run_sanitizers=true
      ;;
    --valgrind)
      run_valgrind=true
      ;;
    --coverage)
      run_coverage=true
      ;;
    --fuzz)
      run_fuzz=true
      ;;
    --docker)
      run_docker=true
      ;;
    --soak-smoke)
      run_soak_smoke=true
      ;;
    --all)
      run_python=true
      run_clang=true
      run_sanitizers=true
      run_valgrind=true
      run_coverage=true
      run_fuzz=true
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
  shift
done

log() {
  printf '\n==> %s\n' "$*"
}

run() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  "$@"
}

run_to_file() {
  local output="$1"
  shift
  printf '+'
  printf ' %q' "$@"
  printf ' > %q\n' "$output"
  "$@" > "$output"
}

find_tool() {
  local override="$1"
  shift
  if [[ -n "$override" ]]; then
    command -v "$override"
    return
  fi
  local candidate
  for candidate in "$@"; do
    if command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return
    fi
  done
  return 1
}

cmake_common=(
  -S "$repo_root"
  -DFPS_BUILD_TESTS=ON
  -DFPS_ENABLE_TUN_TESTS=OFF
  -DFPS_ENABLE_PCAP_TESTS=OFF
)

clang_cxx=""
llvm_cov=""
llvm_profdata=""

ensure_clang() {
  if [[ -z "$clang_cxx" ]]; then
    if ! clang_cxx="$(find_tool "${CLANG_CXX:-}" clang++-20 clang++)"; then
      echo "clang++-20 or clang++ is required" >&2
      exit 2
    fi
  fi
}

ensure_llvm_coverage_tools() {
  ensure_clang
  if [[ -z "$llvm_cov" ]]; then
    if ! llvm_cov="$(find_tool "${LLVM_COV:-}" llvm-cov-20 llvm-cov)"; then
      echo "llvm-cov-20 or llvm-cov is required" >&2
      exit 2
    fi
  fi
  if [[ -z "$llvm_profdata" ]]; then
    if ! llvm_profdata="$(find_tool "${LLVM_PROFDATA:-}" llvm-profdata-20 llvm-profdata)"; then
      echo "llvm-profdata-20 or llvm-profdata is required" >&2
      exit 2
    fi
  fi
}

if [[ "$run_python" == true ]]; then
  log "Script syntax checks"
  run python3 -m py_compile "$repo_root"/tests/integration/*.py "$repo_root"/tools/*.py
  run python3 -c "import websockets"
  run bash -n \
    "$repo_root"/tools/*.sh \
    "$repo_root"/docker/*.sh \
    "$repo_root"/examples/docker/proxy-dante/*.sh
fi

if [[ "$run_docker" == true ]]; then
  image="${FPS_DOCKER_IMAGE:-fps:local}"
  dockerfile="${FPS_DOCKERFILE:-Dockerfile}"
  if [[ "$dockerfile" = /* ]]; then
    dockerfile_path="$dockerfile"
  else
    dockerfile_path="$repo_root/$dockerfile"
  fi
  if [[ ! -f "$dockerfile_path" ]]; then
    echo "FPS_DOCKERFILE does not exist: $dockerfile_path" >&2
    exit 2
  fi
  docker_compiler="${FPS_DOCKER_COMPILER:-gcc}"
  case "$docker_compiler" in
    gcc|clang)
      ;;
    *)
      echo "FPS_DOCKER_COMPILER must be gcc or clang" >&2
      exit 2
      ;;
  esac
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required for --docker" >&2
    exit 2
  fi
  docker_cmd=(docker)
  if [[ "${FPS_DOCKER_SUDO:-0}" == "1" ]]; then
    docker_cmd=(sudo -n docker)
  fi
  if ! "${docker_cmd[@]}" info >/dev/null 2>&1; then
    echo "docker daemon is required for --docker" >&2
    echo "set FPS_DOCKER_SUDO=1 if this environment requires sudo for docker" >&2
    exit 2
  fi
  docker_build_cmd=("${docker_cmd[@]}" build)
  if [[ "${FPS_DOCKER_BUILDKIT:-1}" != "0" ]]; then
    if "${docker_cmd[@]}" buildx version >/dev/null 2>&1; then
      docker_build_cmd=("${docker_cmd[@]}" buildx build --load)
    else
      echo "docker buildx is unavailable; falling back to classic docker build" >&2
    fi
  fi
  log "Docker build/smoke: $image ($dockerfile, $docker_compiler)"
  run "${docker_build_cmd[@]}" \
    -f "$dockerfile_path" \
    --build-arg "FPS_COMPILER=$docker_compiler" \
    -t "$image" "$repo_root"
  run "${docker_cmd[@]}" run --rm "$image" fps_client --help
  run "${docker_cmd[@]}" run --rm "$image" fps_server --help
  run "${docker_cmd[@]}" run --rm "$image" fps_carrier --help
  run "${docker_cmd[@]}" run --rm "$image" iperf3 --version
  docker_tmp="$(mktemp -d)"
  client_uuid="$("${docker_cmd[@]}" run --rm "$image" fps_client --generate-client-uuid)"
  server_keys="$("${docker_cmd[@]}" run --rm "$image" fps_server --generate-server-keypair)"
  server_private_key="$(printf '%s\n' "$server_keys" | sed -n 's/^server_private_key_base64=//p')"
  server_public_key="$(printf '%s\n' "$server_keys" | sed -n 's/^server_public_key_base64=//p')"
  cat > "$docker_tmp/server.json" <<EOF
{
  "network": {
    "listen": "127.0.0.1:17443",
    "target": "127.0.0.1:18443"
  },
  "security": {
    "zero_rtt": {
      "enabled": true,
      "profile_id": "docker-smoke-v5",
      "server_private_key_base64": "$server_private_key",
      "server_public_key_base64": "$server_public_key",
      "allowed_client_uuids": ["$client_uuid"],
      "version": 5,
      "capabilities": 1,
      "max_padding_size": 64,
      "min_records_before_trial": 1,
      "upgrade_direction": "client_to_server"
    }
  }
}
EOF
  run "${docker_cmd[@]}" run --rm \
    -v "$docker_tmp:/etc/fps:ro" \
    "$image" fps_server --check-config --config /etc/fps/server.json
  run "${docker_cmd[@]}" run --rm \
    -e FPS_ROLE=server \
    -v "$docker_tmp:/etc/fps:ro" \
    "$image" check-config
  log "Docker explicit command passthrough smoke"
  set +e
  "${docker_cmd[@]}" run --rm "$image" sh -c 'exit 7'
  passthrough_code=$?
  set -e
  if [[ "$passthrough_code" != "7" ]]; then
    echo "explicit Docker command passthrough returned $passthrough_code, expected 7" >&2
    exit 2
  fi
  rm -rf "$docker_tmp"
  run "${docker_cmd[@]}" compose -f "$repo_root/examples/docker/server/compose.yml" config
  run "${docker_cmd[@]}" compose \
    -f "$repo_root/examples/docker/server/compose.yml" \
    -f "$repo_root/examples/docker/proxy-dante/compose.yml" \
    config
  run "${docker_cmd[@]}" compose -f "$repo_root/examples/docker/client-host/compose.yml" config
  run "${docker_cmd[@]}" compose -f "$repo_root/examples/docker/debug-carrier/compose.yml" config
fi

if [[ "$run_soak_smoke" == true ]]; then
  log "Docker/TUN resilience soak smoke"
  soak_args=(
    "$repo_root/tools/docker_resilience_soak.py"
    --image "${FPS_DOCKER_IMAGE:-fps:local}"
    --duration "${FPS_DOCKER_SOAK_DURATION:-60}"
    --bandwidth "${FPS_DOCKER_SOAK_BANDWIDTH:-500K}"
    --length "${FPS_DOCKER_SOAK_LENGTH:-512}"
  )
  if [[ "${FPS_DOCKER_SOAK_BUILD:-1}" != "0" ]]; then
    soak_args+=(--build)
  fi
  if [[ "${FPS_DOCKER_SOAK_STRESS:-1}" != "0" ]]; then
    soak_args+=(--stress-backpressure)
  fi
  if [[ "${FPS_DOCKER_SOAK_REQUIRE_BACKPRESSURE:-0}" == "1" ]]; then
    soak_args+=(--require-backpressure-event)
  fi
  if [[ "${FPS_DOCKER_SOAK_WITH_SOCKS:-0}" == "1" ]]; then
    soak_args+=(--with-socks-overlay)
  fi
  run "${soak_args[@]}"
fi

if [[ "$run_clang" == true ]]; then
  ensure_clang
  build_dir="$repo_root/cmake-build-clang20"
  log "clang warning build: $build_dir"
  run cmake "${cmake_common[@]}" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="$clang_cxx"
  run cmake --build "$build_dir" -j "$jobs"
  run ctest --test-dir "$build_dir" -L local --output-on-failure
fi

if [[ "$run_sanitizers" == true ]]; then
  ensure_clang
  build_dir="$repo_root/cmake-build-asan-ubsan"
  sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
  log "ASan+UBSan build: $build_dir"
  run cmake "${cmake_common[@]}" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="$clang_cxx" \
    -DCMAKE_CXX_FLAGS="$sanitizer_flags" \
    -DCMAKE_EXE_LINKER_FLAGS="$sanitizer_flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$sanitizer_flags"
  run cmake --build "$build_dir" -j "$jobs"
  run env \
    ASAN_OPTIONS=detect_leaks=1:strict_init_order=1:check_initialization_order=1:abort_on_error=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    ctest --test-dir "$build_dir" -L local --output-on-failure
fi

if [[ "$run_valgrind" == true ]]; then
  if ! command -v valgrind >/dev/null 2>&1; then
    echo "valgrind is required" >&2
    exit 2
  fi
  build_dir="$repo_root/cmake-build-valgrind"
  log "Valgrind unit build: $build_dir"
  run cmake "${cmake_common[@]}" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug
  run cmake --build "$build_dir" -j "$jobs"
  run valgrind \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect \
    --track-origins=yes \
    --error-exitcode=99 \
    "$build_dir/fps_unit_tests" --catch_system_errors=no
fi

if [[ "$run_coverage" == true ]]; then
  ensure_llvm_coverage_tools
  build_dir="$repo_root/cmake-build-coverage"
  profile_dir="$build_dir/profiles"
  profdata="$build_dir/fps.profdata"
  summary="$build_dir/coverage-summary.txt"
  json_summary="$build_dir/coverage-summary.json"
  html_dir="$build_dir/coverage-html"
  coverage_flags="-fprofile-instr-generate -fcoverage-mapping -O0 -g"
  log "Coverage build: $build_dir"
  run cmake "${cmake_common[@]}" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="$clang_cxx" \
    -DCMAKE_CXX_FLAGS="$coverage_flags" \
    -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fprofile-instr-generate"
  run cmake --build "$build_dir" -j "$jobs"
  rm -rf "$profile_dir" "$html_dir"
  mkdir -p "$profile_dir"
  run env "LLVM_PROFILE_FILE=$profile_dir/fps-%p-%m.profraw" \
    ctest --test-dir "$build_dir" -L local --output-on-failure
  if ! compgen -G "$profile_dir/*.profraw" >/dev/null; then
    echo "coverage run produced no .profraw files" >&2
    exit 1
  fi
  run "$llvm_profdata" merge -sparse "$profile_dir"/*.profraw -o "$profdata"

  coverage_objects=(
    "$build_dir/fps_unit_tests"
    --object "$build_dir/fps_client"
    --object "$build_dir/fps_server"
  )
  coverage_filters=(
    --ignore-filename-regex="(^/usr/|.*/tests/|.*/cmake-build-|.*/CMakeFiles/)"
  )
  coverage_sources=(
    --sources "$repo_root/src"
    --sources "$repo_root/include/fps"
  )

  log "llvm-cov report"
  "$llvm_cov" report "${coverage_objects[@]}" \
    --instr-profile="$profdata" \
    "${coverage_filters[@]}" \
    "${coverage_sources[@]}" | tee "$summary"

  run_to_file "$json_summary" "$llvm_cov" export "${coverage_objects[@]}" \
    --instr-profile="$profdata" \
    "${coverage_filters[@]}" \
    "${coverage_sources[@]}" \
    --summary-only

  log "Coverage thresholds"
  python3 - "$json_summary" "${FPS_COVERAGE_MIN_LINES:-70}" "${FPS_COVERAGE_MIN_FUNCTIONS:-80}" <<'PY'
import json
import sys

summary_path = sys.argv[1]
min_lines = float(sys.argv[2])
min_functions = float(sys.argv[3])

with open(summary_path, "r", encoding="utf-8") as handle:
    totals = json.load(handle)["data"][0]["totals"]

line_percent = float(totals["lines"]["percent"])
function_percent = float(totals["functions"]["percent"])
print(f"total line coverage: {line_percent:.2f}% (minimum {min_lines:.2f}%)")
print(f"total function coverage: {function_percent:.2f}% (minimum {min_functions:.2f}%)")

failed = False
if line_percent < min_lines:
    print("line coverage is below threshold", file=sys.stderr)
    failed = True
if function_percent < min_functions:
    print("function coverage is below threshold", file=sys.stderr)
    failed = True
if failed:
    sys.exit(1)
PY

  log "llvm-cov HTML report: $html_dir"
  run "$llvm_cov" show "${coverage_objects[@]}" \
    --instr-profile="$profdata" \
    "${coverage_filters[@]}" \
    "${coverage_sources[@]}" \
    --format=html \
    --output-dir="$html_dir"
  printf 'Coverage summary: %s\n' "$summary"
  printf 'Coverage JSON: %s\n' "$json_summary"
  printf 'Coverage HTML: %s/index.html\n' "$html_dir"
fi

if [[ "$run_fuzz" == true ]]; then
  ensure_clang
  build_dir="$repo_root/cmake-build-fuzz"
  fuzz_flags="-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -g -O1"
  log "libFuzzer smoke build: $build_dir"
  run cmake "${cmake_common[@]}" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_COMPILER="$clang_cxx" \
    -DCMAKE_CXX_FLAGS="$fuzz_flags" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DFPS_BUILD_TESTS=OFF \
    -DFPS_BUILD_FUZZERS=ON
  run cmake --build "$build_dir" -j "$jobs" --target \
    fps_fuzz_tls_records \
    fps_fuzz_covert_codec \
    fps_fuzz_envelope \
    fps_fuzz_zero_rtt \
    fps_fuzz_tun_frames

  fuzz_args=("-runs=${FPS_FUZZ_RUNS:-256}" "-print_final_stats=1")
  if [[ -n "${FPS_FUZZ_SECONDS:-}" ]]; then
    fuzz_args+=("-max_total_time=${FPS_FUZZ_SECONDS}")
  fi
  corpus_work="$build_dir/corpus"
  rm -rf "$corpus_work"
  mkdir -p "$corpus_work"

  run_fuzzer() {
    local target="$1"
    local corpus_name="$2"
    local source_corpus="$repo_root/tests/fuzz/corpus/$corpus_name"
    local working_corpus="$corpus_work/$corpus_name"
    mkdir -p "$(dirname "$working_corpus")"
    cp -a "$source_corpus" "$working_corpus"
    run env \
      ASAN_OPTIONS=detect_leaks=1:strict_init_order=1:check_initialization_order=1:abort_on_error=1 \
      UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
      "$build_dir/$target" \
      "$working_corpus" \
      "${fuzz_args[@]}"
  }

  run_fuzzer fps_fuzz_tls_records tls_records
  run_fuzzer fps_fuzz_covert_codec covert_codec
  run_fuzzer fps_fuzz_envelope envelope
  run_fuzzer fps_fuzz_zero_rtt zero_rtt
  run_fuzzer fps_fuzz_tun_frames tun_frames
fi
