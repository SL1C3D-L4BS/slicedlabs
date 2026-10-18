# tests/fixtures/fabric/tx

Golden data of the transaction manager (WO-050, SL-ARCH-002 ARCH2-R11, FAB-INV-TX-07).

| File | Holds |
| :---- | :---- |
| `golden_revision.json` | the base revision, minted id, revision, root and tx id of a fixed base and patch with id-source seed 42, identical under two clocks; `tests/unit/fabric/tx_determinism_test.cpp` recomputes them on memory_backend and log_backend |

The values are pure functions of the hashed objects (no time enters them). A change to the record, tree, revision, tx
record or patch layouts, to the test schemas or to the tx id derivation changes them: run the test with
`SL_UPDATE_GOLDEN=1`, review the diff, then `python3 -I tools/testing/fixtures.py lock`.
