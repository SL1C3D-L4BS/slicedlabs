#!/usr/bin/env bash
# fetch_archive.sh <url> <out> (WO-019): FOUNDER ONLY. The one place a dependency archive is downloaded, for
# `just deps-lock-refresh`. Refuses unless SL_DEPS_ALLOW_NETWORK=1. Kept outside the justfile, scripts/ and tests/gates/
# on purpose: those are scanned by gates-check and must never fetch. No gate calls this script.
set -euo pipefail

if [ "${SL_DEPS_ALLOW_NETWORK:-}" != 1 ]; then
  echo "fetch_archive: refused: this downloads from the network and is founder-run (SL_DEPS_ALLOW_NETWORK=1)" >&2
  exit 2
fi
if [ "$#" -ne 2 ]; then
  echo "usage: fetch_archive.sh <https url> <out file>" >&2
  exit 2
fi
case "$1" in
  https://*) ;;
  *)
    echo "fetch_archive: only https:// urls are fetched, got '$1'" >&2
    exit 2
    ;;
esac
mkdir -p "$(dirname "$2")"
exec curl --fail --location --silent --show-error --proto '=https' --output "$2" "$1"
