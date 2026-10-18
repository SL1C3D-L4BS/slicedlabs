---
id: WO-901
class: M
repo: slicedlabs
risk: [governed]
goal: "A fixture work order that passes wo-check."
non_goals:
  - "No pushing."
  - "No Kohi source read or copied."
acceptance:
  - cmd: "just doc-check"
    expect_exit: 0
  - cmd: "just test unit"
    expect_exit: 0
  - cmd: "just deps-provider-test vendored"
    expect_exit: 0
paths_allowed:
  - docs/plan/SL-PLAN-901.md
  - tests/governance/fixtures/wo/**
touches_gates: false
budget:
  tokens: 1000000
  usd: 8
  rounds: 2
approver: founder
---
Traces: SL-EP-002 SL-EP-003

Read: the fixture notes.
