#!/usr/bin/env bash
# neg-deps-unpinned-provider (WO-017): `sldeps check --strict` must fail when a provider that is first in ci
# order is unpinned, and name that provider (C03, C12). Read-only: the ci order is overridden with --ci-order.
#   1. control run, ci order system,vendored,cpm,vcpkg: doctest is system-pinned (accept_sha256): must exit 0
#   2. planted run, ci order cpm,vendored,system,vcpkg: doctest via cpm is unpinned (url_hash and git_commit
#      null): must exit non-zero and name doctest and cpm
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

sldeps=(python3 -I tools/deps/sldeps check --strict)

echo "neg-deps-unpinned-provider: control run (system first)"
if ! "${sldeps[@]}" --ci-order system,vendored,cpm,vcpkg >/dev/null; then
  echo "neg-deps-unpinned-provider: FAIL: control run must exit 0 but --strict failed with system first"
  exit 1
fi

echo "neg-deps-unpinned-provider: planted run (cpm first)"
set +e
out="$("${sldeps[@]}" --ci-order cpm,vendored,system,vcpkg 2>&1)"
status=$?
set -e
printf '%s\n' "$out" | grep -v ': note: ' | head -n 3

if [ "$status" -eq 0 ]; then
  echo "neg-deps-unpinned-provider: FAIL: --strict exited 0 with an unpinned provider first in ci order"
  exit 1
fi
if ! printf '%s\n' "$out" | grep -q 'R-DEPS-UNPINNED: doctest via cpm is unpinned'; then
  echo "neg-deps-unpinned-provider: FAIL: --strict failed (exit ${status}) but did not name doctest via cpm"
  exit 1
fi
echo "neg-deps-unpinned-provider: ok (control exit 0, planted exit ${status}, doctest via cpm named)"
