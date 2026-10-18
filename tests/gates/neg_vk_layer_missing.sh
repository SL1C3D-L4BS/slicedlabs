#!/usr/bin/env bash
# neg-vk-layer-missing (WO-016): a ci configure whose SL_VK_LAYER_DIRS holds no validation layer manifest must
# stop with the pacman line; the same preset with the default layer dirs must configure (control, exit 0).
# Trees live under build/neg/ (SL_BUILD_DIR overrides build). No Vulkan device is opened.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

base="${SL_BUILD_DIR:-$root/build}/neg"
work="$base/vk"
ctl="$base/vk-control"
empty="$base/vk-empty-layers"
rm -rf "$work" "$ctl" "$empty"
mkdir -p "$empty"
trap 'rm -rf "$work" "$ctl" "$empty"' EXIT

echo "neg-vk-layer-missing: control configure (ci-clang, default SL_VK_LAYER_DIRS)"
if ! out="$(cmake --preset ci-clang -B "$ctl" 2>&1)"; then
  printf '%s\n' "$out" | tail -n 15
  echo "neg-vk-layer-missing: FAIL: control configure must exit 0"
  exit 1
fi

echo "neg-vk-layer-missing: planted configure (SL_VK_LAYER_DIRS=$empty)"
set +e
out="$(cmake --preset ci-clang -B "$work" -DSL_VK_LAYER_DIRS="$empty" 2>&1)"
status=$?
set -e
printf '%s\n' "$out" | tail -n 8

if [ "$status" -eq 0 ]; then
  echo "neg-vk-layer-missing: FAIL: configure exited 0 with no validation layer manifest"
  exit 1
fi
if ! printf '%s\n' "$out" | grep -qF 'sudo pacman -S vulkan-validation-layers'; then
  echo "neg-vk-layer-missing: FAIL: configure failed (exit $status) without the pacman line"
  exit 1
fi
echo "neg-vk-layer-missing: ok (control exit 0, planted exit $status and the pacman line printed)"
