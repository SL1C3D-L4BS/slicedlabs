#!/usr/bin/env bash
# runtime-lint (WO-077): the Volume 05 source guardrails over engine/runtime and scripts/runtime (tools/runtime/README.md)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec python3 -I tools/runtime/runtime_lint.py "$@"
