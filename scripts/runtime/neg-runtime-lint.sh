#!/usr/bin/env bash
# neg-runtime-lint (WO-077): one planted defect per runtime-lint rule must fail and be named
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec bash tests/gates/neg_runtime_lint.sh "$@"
