#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: tools/run_android_checks.sh [--host] [--docker] [--connected]
       tools/run_android_checks.sh [--managed-device] [--docker-managed-device]
       tools/run_android_checks.sh [--clean-images]

Runs Android build/test checks:
  --host       Run Gradle unit tests and APK assembly with the current host SDK.
  --docker     Build the source-free Android base image and run host checks
               inside it with the current workspace bind-mounted.
               This is the default outside Docker when no mode is specified.
  --connected  Run the instrumented native smoke on an attached Android
               device/emulator through adb.
  --managed-device
               Run the Gradle Managed Device instrumented smoke with the
               current SDK/emulator environment.
  --docker-managed-device
               Build Dockerfile.android and Dockerfile.android-emulator, then
               run --managed-device inside the emulator image through /dev/kvm.
  --clean-images
               Remove dangling Android Docker artifacts. Set
               FPS_ANDROID_CLEAN_TAGS=1 to also remove known FPS Android tags.

Environment:
  ANDROID_HOME              Android SDK root for --host/--connected.
  ANDROID_SDK_ROOT          Android SDK root alias.
  ANDROID_NDK_HOME          Android NDK path, default:
                            $ANDROID_HOME/ndk/28.2.13676358.
  FPS_ANDROID_DOCKER_IMAGE  Source-containing Docker image tag used only when
                            FPS_ANDROID_SOURCE_IMAGE=1, default: fps:android-ci.
  FPS_ANDROID_DOCKERFILE    Dockerfile for --docker, default:
                            Dockerfile.android.
  FPS_ANDROID_BASE_IMAGE    Source-free Android base image tag for
                            --docker and --docker-managed-device, default:
                            fps:android-ci-base.
  FPS_ANDROID_BASE_TARGET   Dockerfile.android target used for
                            FPS_ANDROID_BASE_IMAGE, default:
                            android-gradle-base.
  FPS_ANDROID_EMULATOR_IMAGE
                            Docker image tag for --docker-managed-device,
                            default: fps:android-emulator-ci.
  FPS_ANDROID_EMULATOR_DOCKERFILE
                            Dockerfile for --docker-managed-device, default:
                            Dockerfile.android-emulator.
  FPS_ANDROID_FORCE_DOCKER_REBUILD=1
                            Rebuild Android Docker images even when the target
                            tag already exists. Use after Dockerfile layer,
                            apt/sdk package or base-image changes.
  FPS_ANDROID_SOURCE_IMAGE=1
                            Build and run the legacy source-containing
                            Dockerfile.android final image instead of the
                            source-free base image plus bind mount.
  FPS_ANDROID_CLEAN_DRY_RUN=1
                            Print --clean-images actions without deleting.
  FPS_ANDROID_CLEAN_TAGS=1  Also remove known FPS Android image tags during
                            --clean-images. By default only dangling images are
                            pruned, so useful cache tags stay available.
  FPS_ANDROID_MANAGED_DEVICE_TASK
                            Gradle task for --managed-device, default:
                            :android:app:fpsApi30AtdDebugAndroidTest.
  FPS_ANDROID_EMULATOR_GPU  Gradle emulator GPU mode, default:
                            swiftshader_indirect.
  FPS_DOCKER_SUDO=1         Run docker through sudo -n.
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run_host=false
run_docker=false
run_connected=false
run_managed_device=false
run_docker_managed_device=false
run_clean_images=false

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
    --managed-device)
      run_managed_device=true
      ;;
    --docker-managed-device)
      run_docker_managed_device=true
      ;;
    --clean-images)
      run_clean_images=true
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

docker_image_exists() {
  local image="$1"
  shift
  "$@" image inspect "$image" >/dev/null 2>&1
}

docker_build_image() {
  local image="$1"
  local dockerfile_path="$2"
  shift 2

  if "$@" buildx version >/dev/null 2>&1 && [[ "${FPS_ANDROID_DOCKER_BUILDKIT:-1}" != "0" ]]; then
    run "$@" buildx build --load -f "$dockerfile_path" -t "$image" "${docker_build_args[@]}" "$repo_root"
  else
    run "$@" build -f "$dockerfile_path" -t "$image" "${docker_build_args[@]}" "$repo_root"
  fi
}

docker_remove_image_tag() {
  local image="$1"
  shift

  if docker_image_exists "$image" "$@"; then
    log "Remove existing Android Docker image tag before rebuild: $image"
    run "$@" image rm --no-prune "$image"
  fi
}

ensure_docker_image() {
  local image="$1"
  local dockerfile_path="$2"
  shift 2

  if [[ "${FPS_ANDROID_FORCE_DOCKER_REBUILD:-0}" != "1" ]] && docker_image_exists "$image" "$@"; then
    log "Reuse existing Android Docker image: $image"
    return 0
  fi
  if [[ "${FPS_ANDROID_FORCE_DOCKER_REBUILD:-0}" == "1" ]]; then
    docker_remove_image_tag "$image" "$@"
  fi

  docker_build_image "$image" "$dockerfile_path" "$@"
}

clean_android_images() {
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required for --clean-images" >&2
    exit 2
  fi

  local docker_cmd=(docker)
  if [[ "${FPS_DOCKER_SUDO:-0}" == "1" ]]; then
    docker_cmd=(sudo -n docker)
  fi

  local dry_run=false
  if [[ "${FPS_ANDROID_CLEAN_DRY_RUN:-0}" == "1" ]]; then
    dry_run=true
  fi
  local clean_tags=false
  if [[ "${FPS_ANDROID_CLEAN_TAGS:-0}" == "1" ]]; then
    clean_tags=true
  fi

  local known_images=(
    "${FPS_ANDROID_BASE_IMAGE:-fps:android-ci-base}"
    "${FPS_ANDROID_DOCKER_IMAGE:-fps:android-ci}"
    "${FPS_ANDROID_EMULATOR_IMAGE:-fps:android-emulator-ci}"
    fps:android-gradle-base
  )

  if [[ "$clean_tags" == true ]]; then
    local image
    for image in "${known_images[@]}"; do
      if docker_image_exists "$image" "${docker_cmd[@]}"; then
        if [[ "$dry_run" == true ]]; then
          printf 'would remove image tag: %s\n' "$image"
        else
          run "${docker_cmd[@]}" image rm --no-prune "$image"
        fi
      fi
    done
  fi

  if [[ "$dry_run" == true ]]; then
    run "${docker_cmd[@]}" images --filter dangling=true
  else
    run "${docker_cmd[@]}" image prune -f
  fi
}

set_android_env() {
  export ANDROID_HOME="${ANDROID_HOME:-/opt/android-sdk}"
  export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
  export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/28.2.13676358}"
  export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator:$PATH"
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
    :android:app:assembleRelease \
    :android:app:assembleDebugAndroidTest

  verify_release_native_surface
}

verify_release_native_surface() {
  log "Verify Android release native surface"
  local nm_cmd
  if command -v llvm-nm >/dev/null 2>&1; then
    nm_cmd=(llvm-nm)
  elif command -v nm >/dev/null 2>&1; then
    nm_cmd=(nm)
  else
    echo "llvm-nm or nm is required to verify Android release native symbols" >&2
    exit 2
  fi

  local libs=()
  while IFS= read -r -d '' lib; do
    libs+=("$lib")
  done < <(find "$repo_root/android/app/build/intermediates" -type f -path '*release*' -name libfps_android_native.so -print0 | sort -z)

  if [[ "${#libs[@]}" -eq 0 ]]; then
    echo "Release libfps_android_native.so was not found under android/app/build/intermediates" >&2
    exit 1
  fi

  local forbidden='FpsNativeTestHooks|runClientAuthSmokeForTest|RunZeroRttServerPeerForTest|InjectInboundDatagramForTest|StartFakeCarrierForTest|InstallTunPacketCaptureSinkForTest'
  local lib
  for lib in "${libs[@]}"; do
    if "${nm_cmd[@]}" -D --defined-only "$lib" 2>/dev/null | grep -E "$forbidden" >&2; then
      echo "Forbidden Android test JNI symbol exported by release library: $lib" >&2
      exit 1
    fi
  done
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

run_managed_device_checks() {
  require_android_sdk
  if ! command -v adb >/dev/null 2>&1; then
    echo "adb is required for --managed-device" >&2
    exit 2
  fi
  if ! command -v emulator >/dev/null 2>&1; then
    echo "Android emulator is required for --managed-device" >&2
    exit 2
  fi

  local task="${FPS_ANDROID_MANAGED_DEVICE_TASK:-:android:app:fpsApi30AtdDebugAndroidTest}"
  local gpu="${FPS_ANDROID_EMULATOR_GPU:-swiftshader_indirect}"
  log "Android Gradle Managed Device smoke"
  run "$repo_root/gradlew" \
    --project-dir "$repo_root" \
    "-Pandroid.testoptions.manageddevices.emulator.gpu=$gpu" \
    "$task"
}

run_docker_checks() {
  local image="${FPS_ANDROID_DOCKER_IMAGE:-fps:android-ci}"
  local android_base_image="${FPS_ANDROID_BASE_IMAGE:-fps:android-ci-base}"
  local android_base_target="${FPS_ANDROID_BASE_TARGET:-android-gradle-base}"
  local emulator_image="${FPS_ANDROID_EMULATOR_IMAGE:-fps:android-emulator-ci}"
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

  local docker_build_args=()

  if [[ "${FPS_ANDROID_SOURCE_IMAGE:-0}" == "1" ]]; then
    log "Build source-containing Android Docker image"
    ensure_docker_image "$image" "$dockerfile_path" "${docker_cmd[@]}"

    log "Run Android checks in source-containing Docker image"
    run "${docker_cmd[@]}" run --rm "$image"
    return 0
  fi

  log "Ensure source-free Android base Docker image"
  if [[ "${FPS_ANDROID_FORCE_DOCKER_REBUILD:-0}" == "1" ]]; then
    # The managed-device image extends this base. A forced base rebuild makes
    # the old emulator image stale, so remove its tag before rebuilding the
    # parent image and let the next managed-device run recreate it.
    docker_remove_image_tag "$emulator_image" "${docker_cmd[@]}"
  fi
  docker_build_args=(--target "$android_base_target")
  ensure_docker_image "$android_base_image" "$dockerfile_path" "${docker_cmd[@]}"

  log "Run Android checks in source-free Docker image"
  run "${docker_cmd[@]}" run \
    --rm \
    -v "$repo_root:/workspaces" \
    -w /workspaces \
    "$android_base_image" \
    tools/run_android_checks.sh --host
}

run_docker_managed_device_checks() {
  local android_base_image="${FPS_ANDROID_BASE_IMAGE:-fps:android-ci-base}"
  local android_base_target="${FPS_ANDROID_BASE_TARGET:-android-gradle-base}"
  local base_dockerfile="${FPS_ANDROID_DOCKERFILE:-Dockerfile.android}"
  local emulator_image="${FPS_ANDROID_EMULATOR_IMAGE:-fps:android-emulator-ci}"
  local emulator_dockerfile="${FPS_ANDROID_EMULATOR_DOCKERFILE:-Dockerfile.android-emulator}"
  local base_dockerfile_path
  local emulator_dockerfile_path

  if [[ "$base_dockerfile" = /* ]]; then
    base_dockerfile_path="$base_dockerfile"
  else
    base_dockerfile_path="$repo_root/$base_dockerfile"
  fi
  if [[ "$emulator_dockerfile" = /* ]]; then
    emulator_dockerfile_path="$emulator_dockerfile"
  else
    emulator_dockerfile_path="$repo_root/$emulator_dockerfile"
  fi
  if [[ ! -f "$base_dockerfile_path" ]]; then
    echo "FPS_ANDROID_DOCKERFILE does not exist: $base_dockerfile_path" >&2
    exit 2
  fi
  if [[ ! -f "$emulator_dockerfile_path" ]]; then
    echo "FPS_ANDROID_EMULATOR_DOCKERFILE does not exist: $emulator_dockerfile_path" >&2
    exit 2
  fi
  if [[ ! -e /dev/kvm ]]; then
    echo "/dev/kvm is required for --docker-managed-device" >&2
    exit 2
  fi
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required for --docker-managed-device" >&2
    exit 2
  fi

  local docker_cmd=(docker)
  if [[ "${FPS_DOCKER_SUDO:-0}" == "1" ]]; then
    docker_cmd=(sudo -n docker)
  fi

  local docker_build_args=()
  if [[ "${FPS_ANDROID_FORCE_DOCKER_REBUILD:-0}" != "1" ]] && docker_image_exists "$emulator_image" "${docker_cmd[@]}"; then
    log "Reuse existing Android emulator Docker image: $emulator_image"
  else
    if [[ "${FPS_ANDROID_FORCE_DOCKER_REBUILD:-0}" == "1" ]]; then
      # The emulator image is a child of the base image. Remove it first so the
      # subsequent forced base rebuild does not leave the old base held alive by
      # the old child image.
      docker_remove_image_tag "$emulator_image" "${docker_cmd[@]}"
    fi

    log "Ensure Android source-free base Docker image"
    docker_build_args=(--target "$android_base_target")
    ensure_docker_image "$android_base_image" "$base_dockerfile_path" "${docker_cmd[@]}"

    log "Ensure Android emulator Docker image"
    docker_build_args=(--build-arg "FPS_ANDROID_BASE_IMAGE=$android_base_image")
    ensure_docker_image "$emulator_image" "$emulator_dockerfile_path" "${docker_cmd[@]}"
  fi

  log "Run Android managed-device checks in Docker"
  run "${docker_cmd[@]}" run \
    --rm \
    --device /dev/kvm \
    --group-add "$(stat -c '%g' /dev/kvm)" \
    --shm-size=2g \
    -v "$repo_root:/workspaces" \
    -w /workspaces \
    "$emulator_image" \
    tools/run_android_checks.sh --managed-device
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

if [[ "$run_managed_device" == true ]]; then
  run_managed_device_checks
fi

if [[ "$run_docker_managed_device" == true ]]; then
  run_docker_managed_device_checks
fi

if [[ "$run_clean_images" == true ]]; then
  clean_android_images
fi
