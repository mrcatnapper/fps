#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: tools/run_android_checks.sh [--host] [--docker] [--connected]

Runs Android build/test checks:
  --host       Run Gradle unit tests and APK assembly with the current host SDK.
  --docker     Build Dockerfile.android and run the host checks inside it.
               This is the default outside Docker when no mode is specified.
  --connected  Run the instrumented native smoke on an attached Android
               device/emulator through adb.

Environment:
  ANDROID_HOME              Android SDK root for --host/--connected.
  ANDROID_SDK_ROOT          Android SDK root alias.
  ANDROID_NDK_HOME          Android NDK path, default:
                            $ANDROID_HOME/ndk/28.2.13676358.
  FPS_ANDROID_DOCKER_IMAGE  Docker image tag for --docker, default:
                            fps:android-ci.
  FPS_ANDROID_DOCKERFILE    Dockerfile for --docker, default:
                            Dockerfile.android.
  FPS_DOCKER_SUDO=1         Run docker through sudo -n.
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run_host=false
run_docker=false
run_connected=false

if [[ $# -eq 0 && "${FPS_ANDROID_DOCKER:-0}" == "1" ]]; then
  run_host=true
elif [[ $# -eq 0 ]]; then
  run_docker=true
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      run_host=true
      ;;
    --docker)
      run_docker=true
      ;;
    --connected)
      run_connected=true
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

remove_existing_docker_image() {
  local image="$1"
  shift
  if "$@" image inspect "$image" >/dev/null 2>&1; then
    log "Remove previous Docker image tag: $image"
    run "$@" image rm --no-prune "$image"
  fi
}

set_android_env() {
  export ANDROID_HOME="${ANDROID_HOME:-/opt/android-sdk}"
  export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
  export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/28.2.13676358}"
  export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"
}

require_android_sdk() {
  set_android_env
  if [[ ! -d "$ANDROID_HOME" ]]; then
    echo "ANDROID_HOME does not exist: $ANDROID_HOME" >&2
    exit 2
  fi
  if [[ ! -d "$ANDROID_NDK_HOME" ]]; then
    echo "ANDROID_NDK_HOME does not exist: $ANDROID_NDK_HOME" >&2
    exit 2
  fi
}

run_host_checks() {
  require_android_sdk
  log "Android host Gradle checks"
  run "$repo_root/gradlew" \
    --project-dir "$repo_root" \
    :android:app:testDebugUnitTest \
    :android:app:assembleDebug \
    :android:app:assembleDebugAndroidTest
}

run_connected_checks() {
  require_android_sdk
  if ! command -v adb >/dev/null 2>&1; then
    echo "adb is required for --connected" >&2
    exit 2
  fi
  if ! adb devices | awk 'NR > 1 && $2 == "device" { found = 1 } END { exit found ? 0 : 1 }'; then
    echo "No attached Android device/emulator in 'device' state" >&2
    adb devices >&2 || true
    exit 2
  fi
  log "Android connected native smoke"
  run "$repo_root/gradlew" \
    --project-dir "$repo_root" \
    :android:app:connectedDebugAndroidTest
}

run_docker_checks() {
  local image="${FPS_ANDROID_DOCKER_IMAGE:-fps:android-ci}"
  local dockerfile="${FPS_ANDROID_DOCKERFILE:-Dockerfile.android}"
  local dockerfile_path
  if [[ "$dockerfile" = /* ]]; then
    dockerfile_path="$dockerfile"
  else
    dockerfile_path="$repo_root/$dockerfile"
  fi
  if [[ ! -f "$dockerfile_path" ]]; then
    echo "FPS_ANDROID_DOCKERFILE does not exist: $dockerfile_path" >&2
    exit 2
  fi
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required for --docker" >&2
    exit 2
  fi

  local docker_cmd=(docker)
  if [[ "${FPS_DOCKER_SUDO:-0}" == "1" ]]; then
    docker_cmd=(sudo -n docker)
  fi

  log "Build Android Docker image"
  remove_existing_docker_image "$image" "${docker_cmd[@]}"
  if "${docker_cmd[@]}" buildx version >/dev/null 2>&1 && [[ "${FPS_ANDROID_DOCKER_BUILDKIT:-1}" != "0" ]]; then
    run "${docker_cmd[@]}" buildx build --load -f "$dockerfile_path" -t "$image" "$repo_root"
  else
    run "${docker_cmd[@]}" build -f "$dockerfile_path" -t "$image" "$repo_root"
  fi

  log "Run Android checks in Docker"
  run "${docker_cmd[@]}" run --rm "$image"
}

if [[ "$run_docker" == true ]]; then
  run_docker_checks
fi

if [[ "$run_host" == true ]]; then
  run_host_checks
fi

if [[ "$run_connected" == true ]]; then
  run_connected_checks
fi
