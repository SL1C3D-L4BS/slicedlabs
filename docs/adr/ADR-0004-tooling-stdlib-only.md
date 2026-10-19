---
id: ADR-0004
title: "Repository tooling is Python 3 standard library only"
status: Accepted
date: 2026-10-17
decision_refs: []
supersedes: []
---
# ADR-0004: Repository tooling is Python 3 standard library only

## Context

Each fragmentation invariant has a named enforcement point that must be a runnable check (master plan section 3).
Machine facts (plan of record, 2026-10-17): python 3.14 and shellcheck 0.11 are installed; the factory's
`approve.py` applies security heuristics to diffs, and builds run in a jail.

## Decision

- Repository tooling (`tools/governance`, `tools/deps`, `tools/presets`) MUST be Python 3 standard library only and run as `python3 -I` (plan of record, Decisions table, ADR-0004 row).
- Gates MUST NOT call subprocess or the network (plan of record, Decisions table: keeps the `approve.py` heuristics quiet and the gate jail-safe).
- A script SHOULD add its own directory to `sys.path` because `-I` drops the script directory (machine fact: Python isolated mode).
- Optional libraries, such as `jsonschema` (machine fact: 4.26 installed), MAY be used only to cross-check in a self-test and are never required (plan of record, WO-002).
- Shell scripts MUST pass shellcheck (machine fact: shellcheck 0.11 installed).

## Consequences

- Front matter is a restricted YAML subset parsed by hand (key: value, flow lists, one nested level).
- Tests are `python3 -I -m unittest discover -s tools -p 'test_*.py'` (`just tools-test`).
- Tools that need git or cmake are bash recipes, not Python.

## Alternatives

- PyYAML and `jsonschema` as hard dependencies: rejected, they are not stdlib, so a missing package would turn a
  gate red for a reason unrelated to the repository (plan of record, Decisions table).
- Node or Rust tooling: rejected, python 3.14 and shellcheck 0.11 are verified on the machine (machine fact above).
