#!/usr/bin/env bash
# Configure the debug (clang) and gcc-debug presets for the feature battery, in parallel, in
# build/features-measure/<preset> (not build/<preset>: a parallel build is left alone).
#   run.sh fresh    delete the feature cache first and measure (SL_FEATURES_MEASURE=1: a failing required feature does not stop)
#   run.sh cached   plain configure through the fingerprint cache
# Writes build/features-measure/<preset>.log and <preset>.rc; cmake/features/features.py reads them.
# Always exits 0: features.py judges the outcome.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root" || exit 2
mode="${1:-}"
case "$mode" in fresh | cached) ;; *)
  echo "usage: run.sh fresh|cached" >&2
  exit 2
  ;;
esac

out="$root/build/features-measure"
mkdir -p "$out"
presets=(debug gcc-debug)
pids=()
for p in "${presets[@]}"; do
  (
    if [ "$mode" = fresh ]; then
      rm -rf "$out/$p/features"
      export SL_FEATURES_MEASURE=1
    else
      unset SL_FEATURES_MEASURE
    fi
    cmake --preset "$p" -B "$out/$p" >"$out/$p.log" 2>&1
    echo $? >"$out/$p.rc"
  ) &
  pids+=("$!")
done
for pid in "${pids[@]}"; do
  wait "$pid"
done
exit 0
