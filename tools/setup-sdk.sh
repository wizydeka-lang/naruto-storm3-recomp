#!/usr/bin/env bash
# Clone ReXGlue at the revision this project was built against, apply the
# project's SDK patches, then build and install it (Release and Debug).
# Usage: bash tools/setup-sdk.sh [sdk directory]   (default: ~/rexglue-sdk)
set -euo pipefail

sdk_revision=c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
sdk_dir="${1:-$HOME/rexglue-sdk}"
jobs="$(nproc)"

for tool in git cmake ninja clang-20 clang++-20; do
  command -v "$tool" > /dev/null || {
    printf 'Missing %s. Install Git, CMake 3.25+, Ninja and Clang 20 first.\n' "$tool" >&2
    exit 1
  }
done

if [[ ! -d "$sdk_dir/.git" ]]; then
  git clone https://github.com/rexglue/rexglue-sdk.git "$sdk_dir"
fi
cd -- "$sdk_dir"
if [[ "$(git rev-parse HEAD)" != "$sdk_revision" ]]; then
  [[ -z "$(git status --porcelain --untracked-files=no)" ]] || {
    printf '%s has local changes and is not at %s; use another directory.\n' \
      "$sdk_dir" "$sdk_revision" >&2
    exit 1
  }
  git fetch origin
  git checkout --detach "$sdk_revision"
fi
git submodule update --init --recursive

# Apply each patch once; a patch that already reverses cleanly is in place.
for patch in "$project_root"/patches/*.patch; do
  if git apply --reverse --check "$patch" 2> /dev/null; then
    printf 'Already applied: %s\n' "$(basename "$patch")"
  else
    git apply "$patch"
    printf 'Applied: %s\n' "$(basename "$patch")"
  fi
done

# FidelityFX headers enable the FSR 1 and CAS upscalers, whose shaders ship
# with the SDK. Only the headers are fetched: the full FidelityFX runtime
# (FSR 2/3) needs Windows shader tools, so REXGLUE_ENABLE_FIDELITYFX stays off.
ffx_revision=eee08db1688ac3d1275a70b728f4a8ba22914213
ffx_dir="$sdk_dir/out/fidelityfx-sdk"
if [[ ! -d "$ffx_dir/sdk/include" ]]; then
  git init -q "$ffx_dir"
  git -C "$ffx_dir" remote add origin https://github.com/rexglue/FidelityFX-SDK.git
  git -C "$ffx_dir" sparse-checkout set --no-cone /sdk/include/ /ffx-api/include/
  git -C "$ffx_dir" fetch -q --depth 1 --filter=blob:none origin "$ffx_revision"
  git -C "$ffx_dir" checkout -q FETCH_HEAD
fi

cmake --preset linux-amd64 -DREXGLUE_ENABLE_FIDELITYFX=OFF \
  "-DREXGLUE_FIDELITYFX_SOURCE_DIR=$ffx_dir"
for config in Release Debug; do
  cmake --build out/build/linux-amd64 --config "$config" --parallel "$jobs"
  cmake --install out/build/linux-amd64 --config "$config"
done
printf 'ReXGlue SDK installed in %s/out/install/linux-amd64\n' "$sdk_dir"
