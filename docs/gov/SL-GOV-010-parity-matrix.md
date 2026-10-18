---
id: SL-GOV-010
kind: GOV
title: "Unreal Parity Matrix and Benchmark Policy"
status: Draft
version: 0.1.0
owner: founder
episodes: [002]
depends_on: [SL-GOV-001, SL-GOV-003]
supersedes: []
decisions: []
---
# SL-GOV-010: Unreal Parity Matrix and Benchmark Policy

## Purpose

This document records, for each Unreal Engine 5 system in master plan section 2.7, the named Sliced Labs counterpart,
the owning volume and the measurable obligation that Sliced Labs has to meet. It makes no parity claim: each
"where Sliced Labs must be better" statement of the plan is carried here as an obligation to measure, and it counts as
met only when the Volume 41 benchmark gate passes (master plan section 2.7 and section 6.3, rule 5). Episode 002
establishes the competitive position.

## Status and decisions

Status is Draft (version 0.1.0); approval is the founder's. The matrix has 34 rows, copied from master plan section 2.7 and
linked to the capability register SL-REG-004 (SL-GOV-003). `just capability-check` compares the `sl-data` block below
with the plan and the register and fails with R-CAP-PARITY when they differ.

## Normative text

- GOV10-R01: Every row of master plan section 2.7 MUST have a capability in SL-REG-004 whose `unreal_counterpart` is the row's Unreal system (master plan section 2.7).
- GOV10-R02: A row's obligation MUST be treated as unmet until the Volume 41 benchmark gate passes on the SL-BENCH reference scenes, on the same hardware and at the same visual target (master plan section 2.7).
- GOV10-R03: A parity claim ("rivals Unreal" or similar) MUST NOT appear in any document, release note or marketing text without benchmark evidence from Volume 41 (master plan section 6.3, rule 5).
- GOV10-R04: A benchmark result MUST name the SL-BENCH scene, the hardware configuration and the visual target, and MUST be reproducible from a revision (master plan section 2.7 and release gate 7).
- GOV10-R05: The obligation text of a row MAY be quoted in this document and MUST NOT be turned into a claim of achievement (master plan section 6.3, rule 5).

Benchmark policy: scenes are the SL-BENCH reference scenes; hardware is one stated configuration for both engines;
the visual target is fixed per scene before measurement; the audit is the Volume 41 competitive benchmarking
audit (master plan section 4.1, Volume 41).

| # | Unreal system | Sliced Labs counterpart | Vol | Benchmark obligation | Capability |
| :---- | :---- | :---- | :---- | :---- | :---- |
| 1 | Nanite virtualized geometry | Sliced Cluster Geometry (cluster LOD DAG, GPU-driven, streaming) | 27 | Source mesh stays editable; no separate build step; LOD error visible per cluster | CAP-04-CLUSTER-GEOMETRY |
| 2 | Lumen dynamic GI and reflections | Sliced Radiance (software + hardware RT GI, surface cache) | 27 | Every GI input traceable; quality tier shown per view; no bake step | CAP-04-RADIANCE |
| 3 | Virtual Shadow Maps | Sliced Virtual Shadows | 27 | Page-cache cost shown per light in the profiler | CAP-04-VIRTUAL-SHADOWS |
| 4 | Temporal Super Resolution | Sliced Temporal Upscaler + vendor upscaler adapters | 28 | Upscaler choice is a capability-gated, benchmarked setting | CAP-04-TEMPORAL-UPSCALER |
| 5 | Substrate materials | Sliced Layered Materials (graph-native, slab model) | 09, 28 | One material graph for 2D, 3D, mobile and web with automatic tier lowering | CAP-04-LAYERED-MATERIALS |
| 6 | Path tracer, Movie Render Queue | Sliced Path Tracer + Render Queue | 28, 40 | Final frames carry full provenance and are reproducible from a revision | CAP-04-PATH-TRACER |
| 7 | Landscape, Water | Sliced Terrain + Water Bodies | 29 | Terrain edits transactional and mergeable between users | CAP-05-TERRAIN-WATER |
| 8 | World Partition, One File Per Actor | Sliced World Streaming on canonical entity IDs | 29, 18 | Semantic merges per entity, not per file | CAP-03-WORLD-STREAMING |
| 9 | PCG framework | Sliced Procedural Graphs | 29 | Seeded, deterministic, diffable output with provenance | CAP-05-PROCEDURAL-GRAPHS |
| 10 | Niagara | Sliced VFX Graph (CPU + GPU) | 14 | Same graph IR as materials and gameplay; deterministic mode for replay | CAP-05-VFX-GRAPH |
| 11 | Chaos physics, destruction, cloth | Jolt adapter + native destruction, cloth, fluids, vehicles | 15, 31 | Replay-safe determinism contract per solver | CAP-06-PHYSICS-DESTRUCTION |
| 12 | Groom hair | Sliced Strands | 30 | Authored in-engine; no external groom tool needed | CAP-05-STRANDS |
| 13 | MetaHuman | Sliced Digital Humans (scan import, face rig, LOD, customization) | 30 | Full pipeline inside the editor, editable at every stage | CAP-05-DIGITAL-HUMANS |
| 14 | Control Rig, IK Retargeter | Sliced Rig Graph + Retargeter | 13, 30 | Rig graph shares the typed graph IR | CAP-05-RIG-GRAPH |
| 15 | Motion Matching, Pose Search | Sliced Motion Matching | 30 | Database rebuild is incremental via the build graph | CAP-05-MOTION-MATCHING |
| 16 | Mutable character customization | Sliced Customization Graphs | 30 | Runtime variants are derived artifacts with provenance | CAP-05-CUSTOMIZATION-GRAPHS |
| 17 | Sequencer | Sliced Sequencer | 13 | Same animation data for gameplay and cinematics | CAP-05-SEQUENCER |
| 18 | Blueprints | Sliced Graphs (typed IR → C++ / Luau) | 19 | Graphs compile to native for shipping; diffable and mergeable | CAP-07-TYPED-GRAPHS |
| 19 | Verse / UEFN | Luau extension layer + Sliced creator runtime | 19, 38 | Sandboxed creator content with permissioned APIs | CAP-07-LUAU-CREATOR-RUNTIME |
| 20 | UMG / Slate | Sliced UI (one retained framework) | 06, 14 | Editor and game UI on one framework | CAP-01-UI-FRAMEWORK |
| 21 | MetaSounds | Sliced Sound Graphs | 14, 32 | Shares graph IR; sample-accurate replay | CAP-05-SOUND-GRAPHS |
| 22 | Mass Entity | Native archetype ECS (the core runtime, not an add-on) | 05, 32 | Crowds use the same ECS as everything else | CAP-03-ARCHETYPE-ECS |
| 23 | Gameplay Ability System | Sliced Abilities | 32 | Ability data is schema-native and replay-safe | CAP-06-ABILITIES |
| 24 | Iris replication, Online Subsystem, EOS | Sliced Replication + Sliced Online Services | 15, 32 | Divergence detection built in; services run on Sliced Hub | CAP-06-REPLICATION-ONLINE |
| 25 | Live Coding | Hot-reload protocol with ABI manifest and rollback | 19 | Rollback instead of crash on incompatible change | CAP-07-HOT-RELOAD |
| 26 | Unreal Insights | Sliced Insights (CPU, GPU, memory, net, assets) | 16 | Every sample tied to entity, system, revision and tick (F6) | CAP-09-INSIGHTS |
| 27 | Multi-User Editing, Concert | Native collaboration on the Fabric | 18 | Semantic three-way merges, offline reconciliation | CAP-10-NATIVE-COLLABORATION |
| 28 | Horde, Zen storage | Sliced Build Farm + CAS | 10, 22 | Reproducible artifacts with provenance (F3) | CAP-08-BUILD-FARM-CAS |
| 29 | Datasmith, USD | Sliced Interchange (USD, glTF, FBX, Alembic, MaterialX) | 39 | Imports keep source identity and fidelity reports | CAP-08-INTERCHANGE |
| 30 | Live Link | Sliced Live Link (mocap, face capture, DCC sync) | 39, 40 | Recorded takes become versioned assets | CAP-05-LIVE-LINK |
| 31 | nDisplay, virtual production | Sliced Stage (LED walls, camera tracking, genlock) | 40 | Stage config is part of the project model | CAP-05-STAGE |
| 32 | Pixel Streaming | Sliced Stream (WebRTC cloud rendering) | 36 | Shares the web runtime and Hub accounts | CAP-11-STREAM |
| 33 | Fab marketplace, Quixel library | Sliced Marketplace + asset library | 38 | Packages carry provenance and compatibility proofs | CAP-11-MARKETPLACE |
| 34 | Lyra sample game | Sliced reference games (2D, 3D, multiplayer, VR) | 25, 41 | Each is also a benchmark scene | CAP-11-REFERENCE-GAMES |

Matrix rows from master plan section 2.7, in plan order (the `sl-data` block is the checked copy):

```sl-data
{"benchmark": {"scenes": "SL-BENCH", "hardware": "same", "visual_target": "same", "audit": "Volume 41", "gate": 7},
"rows": [
{"n": 1, "unreal": "Nanite virtualized geometry", "sliced": "Sliced Cluster Geometry (cluster LOD DAG, GPU-driven, streaming)", "volumes": ["27"], "obligation": "Source mesh stays editable; no separate build step; LOD error visible per cluster", "capability": "CAP-04-CLUSTER-GEOMETRY"},
{"n": 2, "unreal": "Lumen dynamic GI and reflections", "sliced": "Sliced Radiance (software + hardware RT GI, surface cache)", "volumes": ["27"], "obligation": "Every GI input traceable; quality tier shown per view; no bake step", "capability": "CAP-04-RADIANCE"},
{"n": 3, "unreal": "Virtual Shadow Maps", "sliced": "Sliced Virtual Shadows", "volumes": ["27"], "obligation": "Page-cache cost shown per light in the profiler", "capability": "CAP-04-VIRTUAL-SHADOWS"},
{"n": 4, "unreal": "Temporal Super Resolution", "sliced": "Sliced Temporal Upscaler + vendor upscaler adapters", "volumes": ["28"], "obligation": "Upscaler choice is a capability-gated, benchmarked setting", "capability": "CAP-04-TEMPORAL-UPSCALER"},
{"n": 5, "unreal": "Substrate materials", "sliced": "Sliced Layered Materials (graph-native, slab model)", "volumes": ["09", "28"], "obligation": "One material graph for 2D, 3D, mobile and web with automatic tier lowering", "capability": "CAP-04-LAYERED-MATERIALS"},
{"n": 6, "unreal": "Path tracer, Movie Render Queue", "sliced": "Sliced Path Tracer + Render Queue", "volumes": ["28", "40"], "obligation": "Final frames carry full provenance and are reproducible from a revision", "capability": "CAP-04-PATH-TRACER"},
{"n": 7, "unreal": "Landscape, Water", "sliced": "Sliced Terrain + Water Bodies", "volumes": ["29"], "obligation": "Terrain edits transactional and mergeable between users", "capability": "CAP-05-TERRAIN-WATER"},
{"n": 8, "unreal": "World Partition, One File Per Actor", "sliced": "Sliced World Streaming on canonical entity IDs", "volumes": ["29", "18"], "obligation": "Semantic merges per entity, not per file", "capability": "CAP-03-WORLD-STREAMING"},
{"n": 9, "unreal": "PCG framework", "sliced": "Sliced Procedural Graphs", "volumes": ["29"], "obligation": "Seeded, deterministic, diffable output with provenance", "capability": "CAP-05-PROCEDURAL-GRAPHS"},
{"n": 10, "unreal": "Niagara", "sliced": "Sliced VFX Graph (CPU + GPU)", "volumes": ["14"], "obligation": "Same graph IR as materials and gameplay; deterministic mode for replay", "capability": "CAP-05-VFX-GRAPH"},
{"n": 11, "unreal": "Chaos physics, destruction, cloth", "sliced": "Jolt adapter + native destruction, cloth, fluids, vehicles", "volumes": ["15", "31"], "obligation": "Replay-safe determinism contract per solver", "capability": "CAP-06-PHYSICS-DESTRUCTION"},
{"n": 12, "unreal": "Groom hair", "sliced": "Sliced Strands", "volumes": ["30"], "obligation": "Authored in-engine; no external groom tool needed", "capability": "CAP-05-STRANDS"},
{"n": 13, "unreal": "MetaHuman", "sliced": "Sliced Digital Humans (scan import, face rig, LOD, customization)", "volumes": ["30"], "obligation": "Full pipeline inside the editor, editable at every stage", "capability": "CAP-05-DIGITAL-HUMANS"},
{"n": 14, "unreal": "Control Rig, IK Retargeter", "sliced": "Sliced Rig Graph + Retargeter", "volumes": ["13", "30"], "obligation": "Rig graph shares the typed graph IR", "capability": "CAP-05-RIG-GRAPH"},
{"n": 15, "unreal": "Motion Matching, Pose Search", "sliced": "Sliced Motion Matching", "volumes": ["30"], "obligation": "Database rebuild is incremental via the build graph", "capability": "CAP-05-MOTION-MATCHING"},
{"n": 16, "unreal": "Mutable character customization", "sliced": "Sliced Customization Graphs", "volumes": ["30"], "obligation": "Runtime variants are derived artifacts with provenance", "capability": "CAP-05-CUSTOMIZATION-GRAPHS"},
{"n": 17, "unreal": "Sequencer", "sliced": "Sliced Sequencer", "volumes": ["13"], "obligation": "Same animation data for gameplay and cinematics", "capability": "CAP-05-SEQUENCER"},
{"n": 18, "unreal": "Blueprints", "sliced": "Sliced Graphs (typed IR → C++ / Luau)", "volumes": ["19"], "obligation": "Graphs compile to native for shipping; diffable and mergeable", "capability": "CAP-07-TYPED-GRAPHS"},
{"n": 19, "unreal": "Verse / UEFN", "sliced": "Luau extension layer + Sliced creator runtime", "volumes": ["19", "38"], "obligation": "Sandboxed creator content with permissioned APIs", "capability": "CAP-07-LUAU-CREATOR-RUNTIME"},
{"n": 20, "unreal": "UMG / Slate", "sliced": "Sliced UI (one retained framework)", "volumes": ["06", "14"], "obligation": "Editor and game UI on one framework", "capability": "CAP-01-UI-FRAMEWORK"},
{"n": 21, "unreal": "MetaSounds", "sliced": "Sliced Sound Graphs", "volumes": ["14", "32"], "obligation": "Shares graph IR; sample-accurate replay", "capability": "CAP-05-SOUND-GRAPHS"},
{"n": 22, "unreal": "Mass Entity", "sliced": "Native archetype ECS (the core runtime, not an add-on)", "volumes": ["05", "32"], "obligation": "Crowds use the same ECS as everything else", "capability": "CAP-03-ARCHETYPE-ECS"},
{"n": 23, "unreal": "Gameplay Ability System", "sliced": "Sliced Abilities", "volumes": ["32"], "obligation": "Ability data is schema-native and replay-safe", "capability": "CAP-06-ABILITIES"},
{"n": 24, "unreal": "Iris replication, Online Subsystem, EOS", "sliced": "Sliced Replication + Sliced Online Services", "volumes": ["15", "32"], "obligation": "Divergence detection built in; services run on Sliced Hub", "capability": "CAP-06-REPLICATION-ONLINE"},
{"n": 25, "unreal": "Live Coding", "sliced": "Hot-reload protocol with ABI manifest and rollback", "volumes": ["19"], "obligation": "Rollback instead of crash on incompatible change", "capability": "CAP-07-HOT-RELOAD"},
{"n": 26, "unreal": "Unreal Insights", "sliced": "Sliced Insights (CPU, GPU, memory, net, assets)", "volumes": ["16"], "obligation": "Every sample tied to entity, system, revision and tick (F6)", "capability": "CAP-09-INSIGHTS"},
{"n": 27, "unreal": "Multi-User Editing, Concert", "sliced": "Native collaboration on the Fabric", "volumes": ["18"], "obligation": "Semantic three-way merges, offline reconciliation", "capability": "CAP-10-NATIVE-COLLABORATION"},
{"n": 28, "unreal": "Horde, Zen storage", "sliced": "Sliced Build Farm + CAS", "volumes": ["10", "22"], "obligation": "Reproducible artifacts with provenance (F3)", "capability": "CAP-08-BUILD-FARM-CAS"},
{"n": 29, "unreal": "Datasmith, USD", "sliced": "Sliced Interchange (USD, glTF, FBX, Alembic, MaterialX)", "volumes": ["39"], "obligation": "Imports keep source identity and fidelity reports", "capability": "CAP-08-INTERCHANGE"},
{"n": 30, "unreal": "Live Link", "sliced": "Sliced Live Link (mocap, face capture, DCC sync)", "volumes": ["39", "40"], "obligation": "Recorded takes become versioned assets", "capability": "CAP-05-LIVE-LINK"},
{"n": 31, "unreal": "nDisplay, virtual production", "sliced": "Sliced Stage (LED walls, camera tracking, genlock)", "volumes": ["40"], "obligation": "Stage config is part of the project model", "capability": "CAP-05-STAGE"},
{"n": 32, "unreal": "Pixel Streaming", "sliced": "Sliced Stream (WebRTC cloud rendering)", "volumes": ["36"], "obligation": "Shares the web runtime and Hub accounts", "capability": "CAP-11-STREAM"},
{"n": 33, "unreal": "Fab marketplace, Quixel library", "sliced": "Sliced Marketplace + asset library", "volumes": ["38"], "obligation": "Packages carry provenance and compatibility proofs", "capability": "CAP-11-MARKETPLACE"},
{"n": 34, "unreal": "Lyra sample game", "sliced": "Sliced reference games (2D, 3D, multiplayer, VR)", "volumes": ["25", "41"], "obligation": "Each is also a benchmark scene", "capability": "CAP-11-REFERENCE-GAMES"}
]}
```

## Measures and enforcement points

- GOV10-R01: `just capability-check` rule R-CAP-PARITY; numerator is plan rows without a matching capability, denominator is plan rows (34), threshold 0.
- GOV10-R02, R04: enforced by the Volume 41 benchmark gate (gate 7), which does not exist yet.
- GOV10-R03, R05: `just capability-check` rule R-CAP-CLAIM scans SL-GOV-003, SL-GOV-010 and SL-REG-004 for the phrases "rivals", "beats" and "better than" outside quoted text; wider scans of release text are not built.

## Traceability

Master plan sections 2.7, 4.1 (Volume 41) and 6.3 rule 5; episode SL-EP-002. Register: SL-REG-004. Companion
document: SL-GOV-003. Tests: `tools/governance/test_check_caps.py`.

## Open issues

- The release gate numbering ("gate 7") is taken from the work order; SL-GOV-007 (WO-009) is the owner of the gate list and has not landed.
- SL-BENCH is named by master plan section 2.7 but has no definition document yet; the scene list belongs to Volume 41.
- GOV10-R02 and R04 have no enforcement point until Volume 41 builds the benchmark gate.
- The work order counted 35 matrix rows; master plan section 2.7 has 34, and this document follows the plan.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft: parity matrix as benchmark obligations, benchmark policy (WO-008). |
