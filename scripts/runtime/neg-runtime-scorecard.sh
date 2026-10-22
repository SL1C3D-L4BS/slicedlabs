#!/usr/bin/env bash
# neg-runtime-scorecard (WO-100): a copy of the inputs with one leak and one sanitizer report must fail
# runtime-scorecard naming leaks and sanitizer_reports (tests/gates/neg_runtime_scorecard.sh)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec bash tests/gates/neg_runtime_scorecard.sh "$@"
