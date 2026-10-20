#!/usr/bin/env bash
# neg-runtime-contracts (WO-077): planted contract drift must fail runtime-contracts-strict and be named
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec bash tests/gates/neg_runtime_contracts.sh "$@"
