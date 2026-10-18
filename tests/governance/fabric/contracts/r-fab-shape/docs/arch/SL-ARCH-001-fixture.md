# Fixture ARCH 1

## Rules

- ARCH1-R01: Fixture rule 1 MUST hold.
- ARCH1-R02: Fixture rule 2 MUST hold.

```sl-data
{"invariants": [
{"id": "FAB-INV-ID-01", "rule": "ARCH1-R01", "text": "fixture invariant", "test": "tests/unit/fabric/id_test.cpp", "wo": "WO-901"},
{"id": "FAB-INV-BY-01", "rule": "ARCH1-R02", "text": "fixture invariant", "test": "tools/fabric/test_by.py", "wo": "WO-902", "verified_by": "just fabric-ok"}],
"retired": []}
```
