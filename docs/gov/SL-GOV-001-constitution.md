---
id: SL-GOV-001
kind: GOV
title: "Engine Constitution and Engineering Doctrine"
status: Draft
version: 0.1.0
owner: founder
episodes: [001, 003, 011, 020]
depends_on: []
supersedes: []
decisions: []
---
# SL-GOV-001: Engine Constitution and Engineering Doctrine

## Purpose

This document is the Engine Constitution: the one-sentence definition, the three founding principles, the four
zeros of the zero-manual-fragmentation doctrine, the locked decisions and the governance rules, each copied from
the master plan sections 1.1, 1.2, 1.3, 1.5 and 6.3. It adds no requirement of its own: what the plan does not
say is listed under Open issues. Episode 001 writes it; episode 020 approves and freezes it (master plan, Volume 01).

## Status and decisions

Status is Draft (version 0.1.0). Approval is episode 020 (WO-023) and belongs to the founder. The 19 locked
decisions of master plan section 1.5 are in the `sl-data` block below. `just constitution-check` compares that
block with the plan and fails with R-CONST-LOCKED when they differ. A change to a locked decision needs an ADR
(master plan section 6.3, rule 3).

## Normative text

Definition (master plan section 1.1): Sliced Labs is a unified interactive production platform in which every
authoritative project fact has one identity, one history, one dependency graph, one transaction model and one
live execution environment, with measured, governed boundaries where external toolchains, platform SDKs or
specialist dependencies are unavoidable.

Principles (master plan section 1.2):

- GOV1-R01: There MUST be one canonical project model: every asset, entity, system, dependency, build configuration, test result and runtime observation has one authoritative identity and a traceable history (master plan section 1.2, principle 1).
- GOV1-R02: There MUST be one continuous production environment: authoring, simulation, debugging, profiling, collaboration, testing, packaging and release share one interaction model, one command system, one transaction protocol and one dependency graph (master plan section 1.2, principle 2).
- GOV1-R03: There MAY be multiple execution environments (editor, runtime, headless test runner, dedicated server, build worker, collaboration service) in separate processes or machines, and they MUST share schemas, execution semantics, artifact identities and evidence contracts (master plan section 1.2, principle 3).

The four zeros (master plan section 1.3). The promise is not "no external program ever runs":

- GOV1-R04: Native-owned workflows MUST have zero manual export/import steps (master plan section 1.3, zero 1).
- GOV1-R05: A project fact MUST NOT have competing authoritative representations (master plan section 1.3, zero 2).
- GOV1-R06: Every source-to-derived transformation MUST be tracked (master plan section 1.3, zero 3).
- GOV1-R07: A stale artifact MUST NOT be presented as current (master plan section 1.3, zero 4).
- GOV1-R08: Compilers, debuggers, GPU drivers, platform SDKs, signing tools, middleware behind adapters, remote workers and interchange formats (glTF, USD, FBX, OpenEXR, MaterialX) MAY be used; each becomes fragmentation only when it creates a second source of truth, an untracked transformation or a manual reconciliation step (master plan section 1.3).

Governance rules (master plan section 6.3):

- GOV1-R09: The JSON episode registry (SL-REG-002) MUST be authoritative; the plan document and any CSV are views of it (master plan section 6.3, rule 1).
- GOV1-R10: Episode IDs MUST NOT change meaning; a changed scope amends the record, and a new capability gets a new ID at the end of its volume's reserved range or in an appendix volume (master plan section 6.3, rule 2).
- GOV1-R11: A scope change MUST carry an impact note and a traceability update, and an ADR when it alters a locked decision; there are no silent scope changes (master plan section 6.3, rule 3).
- GOV1-R12: Claude Code MUST work through bounded work orders (one episode, declared files, declared tests); its report of "done" is not evidence, the test run is (master plan section 6.3, rule 4).
- GOV1-R13: A parity claim ("rivals Unreal" or similar) MUST NOT ship without benchmark evidence from Volume 41 (master plan section 6.3, rule 5).

Locked decisions (master plan section 1.5), rejected items named inside them, and conditional decisions:

```sl-data
{"locked": [
{"area": "Engine language", "decision": "C++23 baseline; C++26 features promoted by toolchain feature tests"},
{"area": "Build system", "decision": "CMake + Ninja for native code; Sliced build graph for content and cooking"},
{"area": "Graphics", "decision": "Vulkan 1.3+ (Windows, Linux, Android); Metal 3 native (macOS, iOS, visionOS); WebGPU (browser); console APIs behind the same backend contract"},
{"area": "Shader language", "decision": "Slang, compiling to SPIR-V, Metal, WGSL and console targets"},
{"area": "Runtime model", "decision": "Archetype ECS; job system with declared read/write access"},
{"area": "Extension language", "decision": "Luau (typed, sandboxable, fast) for tools, mods and gameplay iteration; native C++ for engine and shipping hot paths"},
{"area": "Visual scripting", "decision": "Typed graph IR lowered to C++ for shipping and Luau bytecode for live iteration"},
{"area": "UI", "decision": "Own retained-mode UI framework for editor and games; immediate-mode debug layer on top of it"},
{"area": "Physics", "decision": "Jolt Physics behind an adapter for rigid bodies; native destruction, cloth, fluids and vehicles (Vol 31)"},
{"area": "Audio", "decision": "Native mixer and DSP graph; native procedural audio graph"},
{"area": "Networking", "decision": "Native replication over a reliable-UDP transport; QUIC for services"},
{"area": "Version control", "decision": "Native content-addressed revision store; Git interop for source code"},
{"area": "Web front end", "decision": "TypeScript, React, Next.js"},
{"area": "Web services", "decision": "Rust (axum), PostgreSQL, S3-compatible object storage, NATS JetStream, Kubernetes, OpenTelemetry"},
{"area": "Browser runtime", "decision": "Engine compiled to WebAssembly with the WebGPU backend"},
{"area": "AI", "decision": "Provider-neutral model adapters; local and hosted; every edit through governed transactions"},
{"area": "Hot reload", "decision": "Formal protocol only (section 2.5); arbitrary C++ reload rejected"},
{"area": "Process model", "decision": "One logical platform, multiple processes; single-process-for-everything rejected"},
{"area": "Determinism", "decision": "Contract per subsystem, proven by tests (Vol 17)"}
],
"rejected": [
{"item": "arbitrary C++ reload", "source_row": "Hot reload"},
{"item": "single-process-for-everything", "source_row": "Process model"}
],
"conditional": []}
```

## Measures and enforcement points

- GOV1-R01..R13 are measured as fragmentation invariants F1-F8 in SL-GOV-006 (master plan section 3).
- The locked list is compared with the plan: numerator = locked rows differing from master plan section 1.5 (area or decision text); denominator = 19 rows; threshold 0; gate `just constitution-check`, rule R-CONST-LOCKED.
- Each rejected item MUST name an existing locked row that contains it (gate `just constitution-check`, master plan section 1.5).
- `just doc-check` enforces the document shape and citations; `just contracts-pending` lists contracts whose verifier is still pending and never fails.
- Freezing the locked list with a hash check is `just constitution-frozen` (stub, owner WO-023, episode 020).

## Traceability

Master plan sections 1.1, 1.2, 1.3, 1.5 and 6.3; episodes SL-EP-001 (product), SL-EP-003 (doctrine and F1-F8),
SL-EP-011 (acceptance criteria), SL-EP-020 (approval); work orders WO-006 (this draft) and WO-023 (approval).

## Open issues

- The heading of master plan section 1.5 names locked, conditional and rejected decisions, but its table lists only locked decisions. The plan lists no conditional decision, so `conditional` is empty. The rejected items above are the two named inside rows.
- ADR seeds for episode 020 (candidates, for the founder; this document does not decide them): the C++26 feature-promotion rule (Engine language), console APIs behind the backend contract (Graphics), and the Extension language split between Luau and native C++.
- The three principles and four zeros carry no measures of their own in section 1; F1-F8 in SL-GOV-006 are the plan's measurement of the doctrine.
- Approval status and the freeze hash do not exist yet (episode 020).

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-006). |
