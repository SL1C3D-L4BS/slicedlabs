---
id: SL-GOV-002
kind: GOV
title: "Requirements Catalogue"
status: Draft
version: 0.1.0
owner: founder
episodes: [004, 011]
depends_on: []
supersedes: []
decisions: [ADR-0004]
max_lines: 200
---
# Requirements Catalogue

## Purpose

SL-GOV-002 lists the requirements the master plan states, one stable id each, so that every product promise is
converted to a test and evidence (master plan episode 011) and traced from requirement to architecture to
implementation to test (master plan episode 019). It states nothing the plan does not state. The machine-readable
catalogue is the `sl-data` block under Normative text; `registry/SL-REG-003-traceability.json` carries the same rows
plus their architecture documents, episodes and tests, and `just rtm-check` compares the two.

## Status and decisions

Draft, owner founder, approver founder. ADR-0004 binds the stdlib-only checker. Areas and their master plan sources:
DOCTRINE (sections 1.2 and 1.3), FRAG (section 3, F1-F8, exemptions, scorecard), EXEC (section 2.3), GOV (section 6.3
and the section 6 introduction), QUALITY (episodes 011 and 019, section 6.1), RELEASE (Release gates table), USERS
(episode 004). Every row starts as `planned`; no requirement is implemented or verified yet.

## Normative text

- GOV2-R01: A requirement id MUST be REQ-<AREA>-nnn with AREA one of DOCTRINE, FRAG, EXEC, GOV, QUALITY, RELEASE, USERS, and an id MUST NOT be reused or change meaning (master plan section 6.3, rule 2; episode 019).
- GOV2-R02: Every requirement MUST carry its text, its source in the master plan, and either a measure (numerator, denominator, threshold) or an evidence type (master plan episode 011).
- GOV2-R03: The catalogue MUST NOT hold a requirement the master plan does not state; a gap is an Open issue (master plan section 6.3, rule 3).
- GOV2-R04: Every catalogue requirement MUST have a row in SL-REG-003 and every row MUST have a catalogue requirement (master plan episode 019; Required documents and registries).
- GOV2-R05: Every row MUST name at least one architecture document, one episode and one test, where a test is `just <recipe>` or `deferred:SL-EP-nnn` (master plan episodes 011 and 019).
- GOV2-R06: The F1-F8 measures MUST equal the section 3 table; SL-GOV-006 holds the full invariant specification (master plan section 3).

The catalogue (46 requirements: DOCTRINE 8, FRAG 12, EXEC 3, GOV 6, QUALITY 4, RELEASE 8, USERS 5):

```sl-data
{"requirements": [
 {"id": "REQ-DOCTRINE-001", "text": "Every asset, entity, system, dependency, build configuration, test result and runtime observation MUST have one authoritative identity and a traceable history.", "src": "master plan section 1.2, principle 1", "evidence_type": "architecture conformance suite"},
 {"id": "REQ-DOCTRINE-002", "text": "Authoring, simulation, debugging, profiling, collaboration, testing, packaging and release MUST share one interaction model, one command system, one transaction protocol and one dependency graph.", "src": "master plan section 1.2, principle 2", "evidence_type": "architecture conformance suite"},
 {"id": "REQ-DOCTRINE-003", "text": "Editor, runtime, headless test runner, dedicated server, build worker and collaboration service MAY run in separate processes or machines but MUST share schemas, execution semantics, artifact identities and evidence contracts.", "src": "master plan section 1.2, principle 3", "evidence_type": "cross-process contract tests"},
 {"id": "REQ-DOCTRINE-004", "text": "Native-owned workflows MUST have zero manual export/import steps.", "src": "master plan section 1.3, first zero", "evidence_type": "measured by REQ-FRAG-001"},
 {"id": "REQ-DOCTRINE-005", "text": "No fact MUST have competing authoritative representations.", "src": "master plan section 1.3, second zero", "evidence_type": "measured by REQ-FRAG-002"},
 {"id": "REQ-DOCTRINE-006", "text": "Every source-to-derived transformation MUST be tracked.", "src": "master plan section 1.3, third zero", "evidence_type": "measured by REQ-FRAG-003"},
 {"id": "REQ-DOCTRINE-007", "text": "No stale artifact MUST be presented as current.", "src": "master plan section 1.3, fourth zero", "evidence_type": "measured by REQ-FRAG-004"},
 {"id": "REQ-DOCTRINE-008", "text": "External programs (compilers, debuggers, GPU drivers, platform SDKs, signing tools, middleware behind adapters, remote workers, interchange formats) MAY run; one counts as fragmentation only when it creates a second source of truth, an untracked transformation or a manual reconciliation step.", "src": "master plan section 1.3, closing paragraph", "evidence_type": "constitution text equals the plan"},
 {"id": "REQ-FRAG-001", "text": "No manual export/import in native workflows (F1); enforced by the workflow acceptance suite and a build graph that rejects undeclared inputs.", "src": "master plan section 3, F1", "measure": {"num": "Workflow-test steps requiring an external app", "den": "Total steps, per discipline acceptance workflow", "threshold": "0"}},
 {"id": "REQ-FRAG-002", "text": "No duplicate authoritative representations (F2); the identity registry rejects a second ownership claim.", "src": "master plan section 3, F2", "measure": {"num": "Data domains with more than one registered owner", "den": "All domains", "threshold": "0"}},
 {"id": "REQ-FRAG-003", "text": "Complete provenance (F3); the artifact store refuses writes without the provenance record.", "src": "master plan section 3, F3", "measure": {"num": "Derived artifacts missing any of input IDs, input revisions, toolchain ID, parameters, environment hash", "den": "All derived artifacts", "threshold": "0"}},
 {"id": "REQ-FRAG-004", "text": "No stale artifact shown as current (F4); a currency check runs on every artifact read.", "src": "master plan section 3, F4", "measure": {"num": "Artifacts served while an input revision changed or is unverifiable", "den": "Artifacts served", "threshold": "0"}},
 {"id": "REQ-FRAG-005", "text": "Every edit is transactional (F5); mutation APIs are reachable only through the transaction manager.", "src": "master plan section 3, F5", "measure": {"num": "Mutations bypassing the transaction manager (instrumented in debug builds)", "den": "All mutations", "threshold": "0"}},
 {"id": "REQ-FRAG-006", "text": "Observations are traceable (F6); process-wide events carry a scope tag instead of an entity.", "src": "master plan section 3, F6", "measure": {"num": "Trace events missing correlation fields", "den": "All events", "threshold": "0 missing required fields"}},
 {"id": "REQ-FRAG-007", "text": "Minimal rebuild scope (F7), reported by the build graph report.", "src": "master plan section 3, F7", "measure": {"num": "Rebuilt nodes", "den": "Dependency-closure nodes on the reference workload set", "threshold": "<= 1.05 (baseline set in episode 198)"}},
 {"id": "REQ-FRAG-008", "text": "No external tool needed for final content in core disciplines (F8), at 1.0 for every core discipline.", "src": "master plan section 3, F8", "measure": {"num": "Core disciplines whose acceptance workflow needs an external app", "den": "Core disciplines", "threshold": "0 at 1.0 for every core discipline"}},
 {"id": "REQ-FRAG-009", "text": "Each exemption MUST be registered in SL-REG-006, shown in the editor and reviewed every release; the allowed classes are console certification, platform signing, specialist simulation solvers, legal or regulatory validation, interchange import/export and AI model training.", "src": "master plan section 3.1", "evidence_type": "exception register entries"},
 {"id": "REQ-FRAG-010", "text": "A metric over its threshold with no registered exemption MUST give an editor warning and diagnostic record while authoring, a configurable failure in local validation, and a blocked release.", "src": "master plan section 3.2", "evidence_type": "scorecard behaviour tests"},
 {"id": "REQ-FRAG-011", "text": "A release MUST pass only when every applicable invariant F1-F8 meets its threshold.", "src": "master plan section 3, introduction", "evidence_type": "fragmentation scorecard"},
 {"id": "REQ-FRAG-012", "text": "The editor MUST surface a scorecard of native-authored content coverage per discipline, provenance completeness (F3), over-invalidation ratio (F7), stale-artifact incidents (F4), transaction coverage (F5), cross-process boundary count and latency cost, and external-tool dependency surface with each exemption listed.", "src": "master plan section 3.3", "evidence_type": "editor scorecard acceptance test"},
 {"id": "REQ-EXEC-001", "text": "Local authoring, preview and core builds MUST work with no remote service running.", "src": "master plan section 2.3", "evidence_type": "offline workflow test"},
 {"id": "REQ-EXEC-002", "text": "Remote pieces (build workers, collaboration service, AI inference) MUST add scale and never authority.", "src": "master plan section 2.3", "evidence_type": "architecture review and remote-worker tests"},
 {"id": "REQ-EXEC-003", "text": "The editor, runtime preview, headless test runner, build workers, collaboration service and shipped runtime MUST share contracts: IDs, schemas, event envelopes and evidence records.", "src": "master plan section 2.3", "evidence_type": "cross-process contract tests"},
 {"id": "REQ-GOV-001", "text": "The JSON episode registry SL-REG-002 MUST be authoritative; the master plan and any CSV are views of it.", "src": "master plan section 6.3, rule 1", "evidence_type": "registry checks"},
 {"id": "REQ-GOV-002", "text": "Episode IDs MUST never change meaning; a changed scope amends the record and a new capability gets a new ID.", "src": "master plan section 6.3, rule 2", "evidence_type": "meaning hash comparison"},
 {"id": "REQ-GOV-003", "text": "No scope change MUST be silent: every change carries an impact note, a traceability update and an ADR when it alters a locked decision.", "src": "master plan section 6.3, rule 3", "evidence_type": "registry rule R-SILENT-SCOPE"},
 {"id": "REQ-GOV-004", "text": "Claude Code MUST work through bounded work orders (one episode, declared files, declared tests); its report of done is not evidence, the test run is.", "src": "master plan section 6.3, rule 4", "evidence_type": "work-order check"},
 {"id": "REQ-GOV-005", "text": "No parity claim (rivals Unreal or similar) MUST ship without benchmark evidence from Volume 41.", "src": "master plan section 6.3, rule 5", "evidence_type": "audited benchmark register"},
 {"id": "REQ-GOV-006", "text": "An episode MUST be accepted only on evidence: passing acceptance tests, an integrated demo in the running product and a completion record.", "src": "master plan section 6, introduction", "evidence_type": "evidence verification"},
 {"id": "REQ-QUALITY-001", "text": "Product promises MUST be converted to measurable acceptance criteria, each with a measure or evidence type and at least one test.", "src": "master plan episode 011", "evidence_type": "register completeness check"},
 {"id": "REQ-QUALITY-002", "text": "Every requirement MUST trace from requirement to architecture to implementation to test in the master traceability matrix.", "src": "master plan episode 019", "evidence_type": "traceability check"},
 {"id": "REQ-QUALITY-003", "text": "Every episode record MUST hold an acceptance suite of positive, negative, boundary, recovery and performance cases.", "src": "master plan section 6.1, Acceptance suite", "evidence_type": "episode record review"},
 {"id": "REQ-QUALITY-004", "text": "Every episode record MUST end in a completion record of revision, test evidence, benchmarks, open defects and approved exceptions.", "src": "master plan section 6.1, Completion record", "evidence_type": "evidence verification"},
 {"id": "REQ-RELEASE-001", "text": "Release gate 1 MUST pass when: every authoritative record has owner, stable ID and revisions, and F1-F8 are at threshold.", "src": "master plan Release gates table, gate 1", "evidence_type": "conformance suite, fragmentation scorecard"},
 {"id": "REQ-RELEASE-002", "text": "Release gate 2 MUST pass when: reference games (2D, 3D multiplayer, mobile, XR, console, web) and a digital human are made entirely in Sliced Labs.", "src": "master plan Release gates table, gate 2", "evidence_type": "reference projects and F8 workflow tests"},
 {"id": "REQ-RELEASE-003", "text": "Release gate 3 MUST pass when: reproducible builds, migrations, crash recovery, soak and security tests pass.", "src": "master plan Release gates table, gate 3", "evidence_type": "qualification report"},
 {"id": "REQ-RELEASE-004", "text": "Release gate 4 MUST pass when: every target passes conformance, store and certification pre-checks.", "src": "master plan Release gates table, gate 4", "evidence_type": "platform reports"},
 {"id": "REQ-RELEASE-005", "text": "Release gate 5 MUST pass when: Hub, Web Studio, Stream, marketplace and online services meet their SLOs.", "src": "master plan Release gates table, gate 5", "evidence_type": "SLO dashboards, pen test"},
 {"id": "REQ-RELEASE-006", "text": "Release gate 6 MUST pass when: install, update, support, incident and rollback procedures are drilled.", "src": "master plan Release gates table, gate 6", "evidence_type": "drill records"},
 {"id": "REQ-RELEASE-007", "text": "Release gate 7 MUST pass when: every parity-matrix row matches or beats Unreal Engine 5 on SL-BENCH.", "src": "master plan Release gates table, gate 7", "evidence_type": "audited benchmark register"},
 {"id": "REQ-RELEASE-008", "text": "Sliced Labs 1.0 MUST ship only when all seven gates pass with recorded evidence on every platform in section 2.6.", "src": "master plan Release gates, introduction", "evidence_type": "recorded evidence for all seven gates"},
 {"id": "REQ-USERS-001", "text": "Solo developers MUST be a defined user class with documented production scenarios.", "src": "master plan episode 004", "evidence_type": "production-scenario definition in SL-VOL-01-SPEC"},
 {"id": "REQ-USERS-002", "text": "Studios MUST be a defined user class with documented production scenarios.", "src": "master plan episode 004", "evidence_type": "production-scenario definition in SL-VOL-01-SPEC"},
 {"id": "REQ-USERS-003", "text": "Technical artists MUST be a defined user class with documented production scenarios.", "src": "master plan episode 004", "evidence_type": "production-scenario definition in SL-VOL-01-SPEC"},
 {"id": "REQ-USERS-004", "text": "Simulation engineers MUST be a defined user class with documented production scenarios.", "src": "master plan episode 004", "evidence_type": "production-scenario definition in SL-VOL-01-SPEC"},
 {"id": "REQ-USERS-005", "text": "Platform developers MUST be a defined user class with documented production scenarios.", "src": "master plan episode 004", "evidence_type": "production-scenario definition in SL-VOL-01-SPEC"}
]}
```

## Measures and enforcement points

- GOV2-R01, R04, R05: `just rtm-check` (rules R-RTM-ID, R-RTM-MISSING, R-RTM-ORPHAN, R-RTM-EPISODE, R-RTM-ARCH, R-RTM-TEST); `just neg-rtm` plants a dangling episode id and expects R-RTM-EPISODE.
- GOV2-R02: `just rtm-check` (R-RTM-SHAPE, R-RTM-EVIDENCE, R-RTM-SRC, R-RTM-DRIFT between this block and the register).
- GOV2-R06: the row measures are the section 3 numerators, denominators and thresholds; `just constitution-check` compares the SL-GOV-006 block with the plan.
- The document itself: `just doc-check`; its registration: `just registry-docs-check`.
- Measure of the catalogue: requirements in the register with at least one test / requirements in this block, threshold 1 (`just rtm-check`).

## Traceability

Master plan sections 1.2, 1.3, 2.3, 3, 6.1, 6.3 and the Release gates table; episodes SL-EP-004, SL-EP-011 and SL-EP-019
(Volume 01). Register: SL-REG-003. Neighbours: SL-GOV-006 (F1-F8), SL-GOV-007 (release gates), SL-GOV-009 (quality
model), SL-GOV-010 (parity and benchmarks). Architecture documents named in the rows are Planned in SL-REG-001.

## Open issues

- The sl-reg-003 schema (WO-002) has id `SL-REQ-nnn` and no text, src, measure or evidence_type fields, so the register rows do not validate against it; the schema needs those fields and the REQ-<AREA>-nnn id pattern.
- The plan has no section on non-functional or per-discipline requirements beyond F1-F8 and the gates; none were invented. Quality attributes of episode 007 (reliability, performance, usability, accessibility, security, compatibility) have no measures yet.
- EXEC-001 and EXEC-002 have no dedicated acceptance episode in the plan; the mapped episodes are the nearest (354, 407, 424, 451).
- GOV-004 (work orders) maps to no Volume 01 or 03 episode; the plan states the rule only in section 6.3.
- Most tests are `deferred:SL-EP-nnn` because the enforcing episodes are not built; they are replaced by `just` recipes as those land.
- Release gates 2 and 4 to 6 give the evidence but not a numeric threshold; the rows carry an evidence type only.
- Status values beyond `planned` (specified, implemented, verified) have no stated transition rule in the plan.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft: 46 requirements from the master plan, registered in SL-REG-003 (WO-007). |
