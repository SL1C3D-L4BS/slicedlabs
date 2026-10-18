#!/usr/bin/env bash
# make_fixture.sh <out.tar> (WO-019): the offline `file://` archive the cpm provider test fetches. A deterministic ustar
# of third_party/doctest/doctest/doctest.h under doctest-2.5.3/ (a GitHub-style single top-level directory, which
# FetchContent strips): fixed member order, mtime 2000-01-01T00:00:00Z, owner 0:0, mode 0644/0755, no compression. The
# same doctest.h therefore gives the same sha256 on every host; fixture.json records it (and the doctest.h hash it was
# built from). `make_fixture.sh --print-hash` prints the sha256 of a fresh build without keeping the archive.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
src="$root/third_party/doctest/doctest/doctest.h"
stage="$(mktemp -d "${TMPDIR:-/tmp}/sl-cpm-fixture.XXXXXX")"
trap 'rm -rf "$stage"' EXIT

out="${1:-}"
if [ -z "$out" ]; then
  echo "usage: make_fixture.sh <out.tar | --print-hash>" >&2
  exit 2
fi
print_hash=0
if [ "$out" = "--print-hash" ]; then
  print_hash=1
  out="$stage/fixture.tar"
fi

mkdir -p "$stage/tree/doctest-2.5.3/doctest"
cp "$src" "$stage/tree/doctest-2.5.3/doctest/doctest.h"
chmod 0644 "$stage/tree/doctest-2.5.3/doctest/doctest.h"
chmod 0755 "$stage/tree/doctest-2.5.3" "$stage/tree/doctest-2.5.3/doctest"
tar --format=ustar --sort=name --mtime='2000-01-01 00:00:00 UTC' --owner=0 --group=0 --numeric-owner \
  -C "$stage/tree" -cf "$out" doctest-2.5.3
if [ "$print_hash" -eq 1 ]; then
  sha256sum "$out" | cut -d' ' -f1
fi
