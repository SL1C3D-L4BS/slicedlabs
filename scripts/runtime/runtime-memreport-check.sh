#!/usr/bin/env bash
# runtime-memreport-check [file...] [--no-leaks] (WO-085): validate sl-runtime-memreport/1 reports against
# schemas/runtime/memreport.schema.json plus semantics (tools/runtime/memreport_check.py); default files
# build/runtime/test/memreport.json and build/runtime/gate/memreport.json. Writes nothing.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec python3 -I tools/runtime/memreport_check.py "$@"
