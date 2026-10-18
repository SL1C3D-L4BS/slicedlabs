#!/usr/bin/env bash
# build.sh (WO-033): build the Sliced Labs development image from scripts/dev-image/Containerfile.
# Usage: scripts/dev-image/build.sh
#   Refuses without podman (prints the command to run). With podman it builds
#   localhost/slicedlabs-dev:<first 12 hex of the sha256 of toolchain/lock.json>.
#   SL_DEV_IMAGE_BASE overrides the base image (record a digest pin here).
# The image needs the network and is not built on the workstation by any gate.
set -eu

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
lock="$root/toolchain/lock.json"
[ -f "$lock" ] || {
  echo "build.sh: missing $lock (run scripts/doctor --write-lock)" >&2
  exit 2
}

sha="$(sha256sum "$lock" | cut -c1-12)"
tag="localhost/slicedlabs-dev:$sha"
args=(build --file "$root/scripts/dev-image/Containerfile" --tag "$tag")
if [ -n "${SL_DEV_IMAGE_BASE:-}" ]; then
  args+=(--build-arg "BASE=$SL_DEV_IMAGE_BASE")
fi
args+=("$root")

if ! command -v podman >/dev/null 2>&1; then
  echo "build.sh: podman is not installed; the image is not built here." >&2
  echo "Install it (sudo pacman -Syu --needed podman) and run:" >&2
  echo "  podman ${args[*]}" >&2
  exit 1
fi

exec podman "${args[@]}"
