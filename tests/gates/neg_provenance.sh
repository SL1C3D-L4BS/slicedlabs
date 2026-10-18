#!/usr/bin/env bash
# neg-provenance (WO-029): a source edited after the build must make provenance-check fail and name the stale input.
# Works on a throwaway COPY of the working tree under build/neg/provenance (no commit needed):
#   1. copy the tree (rsync, build/ and .git left out), configure and build the debug preset, run provenance-check:
#      the control must exit 0
#   2. append a comment to engine/core/src/version.cpp WITHOUT rebuilding, run provenance-check again: it must exit
#      non-zero, print R-PROV-INPUT-STALE and name engine/core/src/version.cpp
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/provenance"
victim="engine/core/src/version.cpp"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
rsync -a --exclude /build --exclude /.git "$root"/ "$work"/

cd "$work"
echo "neg-provenance: control run (clean copy, built)"
if ! { cmake --preset debug && cmake --build --preset debug; } >build.log 2>&1; then
  tail -n 15 build.log
  echo "neg-provenance: FAIL: the clean copy must configure and build"
  exit 1
fi
if ! python3 -I tools/provenance/check.py >control.log 2>&1; then
  cat control.log
  echo "neg-provenance: FAIL: control provenance-check must exit 0 on the freshly built copy"
  exit 1
fi

echo "// edited after the build" >>"$victim"
echo "neg-provenance: planted run (edited $victim after the build, no rebuild)"
set +e
python3 -I tools/provenance/check.py >planted.log 2>&1
status=$?
set -e
cat planted.log
if [ "$status" -eq 0 ]; then
  echo "neg-provenance: FAIL: provenance-check exited 0 with $victim edited after the build"
  exit 1
fi
if ! grep -q "R-PROV-INPUT-STALE.*$victim" planted.log; then
  echo "neg-provenance: FAIL: provenance-check failed (exit $status) without naming R-PROV-INPUT-STALE and $victim"
  exit 1
fi
echo "neg-provenance: ok (control exit 0, planted exit $status, R-PROV-INPUT-STALE names $victim)"
