# tools/diagnostics

Developer diagnostics tooling (WO-030, SL-EP-056). Python 3 stdlib only, run with `python3 -I`, no subprocess
(ADR-0004).

- `log_schema_check.py` (`just log-schema-check`): validates every line of a JSON Lines log written by
  `sl::log::json_sink` (`engine/core/include/sl/core/log.hpp`) against `schemas/sl-log-event.schema.json` with the
  subset validator in `tools/governance/slib.py`. Rules `R-LOG-LOAD`, `R-LOG-JSON`, `R-LOG-SCHEMA` (names the JSON
  pointer), `R-LOG-EMPTY`, `R-LOG-COVERAGE`. The recipe builds the debug preset and runs `sl_tests` with
  `SL_LOG_JSON_PATH=build/debug/log-events.jsonl` first; the checker itself only reads the file.
- `test_log_schema_check.py`: its unit tests (`just tools-test`).

Negative test: `just neg-log-schema` (`tests/gates/neg_log_schema.sh`) plants an event whose `system` is a number and
asserts the check fails naming `/system`.
