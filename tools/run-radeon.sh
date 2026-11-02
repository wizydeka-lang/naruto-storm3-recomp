#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
sdk_prefix="${NARUTO_SDK_PREFIX:-$HOME/rexglue-sdk/out/install/linux-amd64}"
build_type=release
profile=false
check=false
gpu=nvidia
native_only=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug) build_type=debug ;;
    --profile) profile=true ;;
    --check) check=true ;;
    --radeon) gpu=radeon ;;
    --nvidia) gpu=nvidia ;;
    --native-only) native_only=true ;;
    --help|-h)
      printf '%s\n' 'Usage: bash tools/run-radeon.sh [--debug] [--profile] [--check] [--radeon|--nvidia] [--native-only]' \
        'Default: optimized Release build, NVIDIA RTX 2050, PipeWire audio.' \
        '--radeon: use the integrated Radeon 660M instead (menus run at ~25 FPS vs 30).' \
        '--profile: isolated run with per-guest-update GPU logs under out/performance/runs/.' \
        '--check: verify required files without launching or writing files.' \
        '--native-only: draw only in the native view; Xenia runs the command stream but renders nothing.'
      exit 0 ;;
    *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
  esac
  shift
done

build_dir="$project_root/out/build/linux-amd64-$build_type"
suffix=""
[[ "$build_type" == debug ]] && suffix=d
plugin="librexgpu-xenos${suffix}.so"
driver=/usr/share/vulkan/icd.d/${gpu}_icd.x86_64.json
for required in "$build_dir/naruto" "$build_dir/$plugin" \
    "$sdk_prefix/lib64/librexruntime${suffix}.so" \
    "$project_root/assets/default.xex" "$driver" \
    "$project_root/config/naruto-performance.toml"; do
  [[ -r "$required" ]] || {
    printf 'Missing: %s\nBuild the matching SDK and game first.\n' "$required" >&2
    exit 1
  }
done
[[ -x "$build_dir/naruto" ]] || { printf 'Game is not executable.\n' >&2; exit 1; }
if [[ "$check" == true ]]; then
  printf 'Ready: %s\nGPU: %s driver only\nAudio: %s\nProfile logging: %s\n' \
    "$build_dir/naruto" "$gpu" "${SDL_AUDIO_DRIVER:-pipewire}" "$profile"
  exit 0
fi

run_dir="$build_dir"
if [[ "$profile" == true ]]; then
  # Separate executable/config/log paths keep diagnostic settings out of the
  # normal gameplay configuration. Game data and user-save locations are retained.
  run_dir="$project_root/out/performance/runs/$(date -u +%Y%m%dT%H%M%SZ)-$$-$build_type"
  mkdir -p "$run_dir"
  cp --reflink=auto "$build_dir/naruto" "$build_dir/$plugin" "$run_dir/"
  sed 's/^gpu = "info"$/gpu = "debug"/' \
    "$project_root/config/naruto-performance.toml" > "$run_dir/naruto.toml"
  printf 'Profiling guest-output updates; logs: %s/logs\n' "$run_dir"
fi

export VK_DRIVER_FILES="$driver"
export VK_ICD_FILENAMES="$driver"
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-pipewire}"
export SDL_VIDEO_DRIVER=x11
export LD_LIBRARY_PATH="$sdk_prefix/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
native_args=()
if [[ "$native_only" == true ]]; then
  export NARUTO_NATIVE_VIEW=1
  native_args=(--vulkan_skip_rendering=true)
fi
cd -- "$project_root"
exec "$run_dir/naruto" \
  "--game_data_root=$project_root/assets" --gpu_plugin=xenos --vulkan_device=0 \
  --async_shader_compilation=true --log_level=info \
  --vulkan_allow_present_mode_immediate=false \
  --vulkan_allow_present_mode_mailbox=false \
  --vulkan_allow_present_mode_fifo_relaxed=false \
  --vulkan_validation_enabled=false --gpu_debug_markers=false "${native_args[@]}"
