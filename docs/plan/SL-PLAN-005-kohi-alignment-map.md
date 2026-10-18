---
id: SL-PLAN-005
kind: PLAN
title: "Kohi Alignment Map"
status: Draft
version: 0.1.0
owner: founder
episodes: []
depends_on: []
supersedes: []
decisions: [ADR-0004]
---
# Kohi Alignment Map

## Purpose

This document maps the Kohi build-along arcs to Sliced Labs episodes, fixes the weekly build-along loop and states
the clean-room rule (master plan sections 5, 5.1 and 5.2). Kohi is a guide to the order of work. It is never a source
of code for Sliced Labs.

## Status and decisions

Draft, owner founder, who approves it. The arc table below is rendered from the `kohi_ref` fields of
`registry/SL-REG-002-episodes.json` by `just registry-views` (`registry/views/kohi-map.md`, WO-011); the per-episode
list with `clean_room: true` is in that file. Kohi episode numbers follow a community index of the series and are
approximate (master plan section 5.1). Tooling is stdlib-only (ADR-0004).

## Normative text

- PLAN5-R01: Kohi MUST be watched as a build-along, and its source MUST NOT be read, copied, translated or adapted into Sliced Labs (master plan section 5; clean-room rule of this document).
- PLAN5-R02: Every episode with a `kohi_ref` carries `clean_room: true`; the value is listed per episode in `registry/views/kohi-map.md` (master plan section 5.1).
- PLAN5-R03: Sliced Labs SHOULD follow the Kohi order of work but MUST write everything in C++23 with its own architecture (master plan section 5).
- PLAN5-R04: This document MUST NOT contain code fences or engine code (master plan section 5, clean-room rule).
- PLAN5-R05: Volumes 04, 17, 18 and 20 have no Kohi counterpart and MUST be designed from the master plan alone (master plan section 5.1).
- PLAN5-R06: The weekly loop is: watch one Kohi arc; write the episode spec (purpose, contract, tests); paste the ready prompt into Claude Code; build in C++23; if the acceptance tests fail, build again; when they pass, record evidence and tick the episode (master plan section 5.2).
- PLAN5-R07: The core systems SHOULD be typed by hand at least once, even when Claude Code drafts them (master plan section 5.2).

Arc table (generated from `kohi_ref`; section 5.1 adds the deliberate differences per arc):

| Kohi arc | Sliced Labs episodes | Count |
| :---- | :---- | :---- |
| Architecture overview and setup (#000–#002) | 041-048 | 8 |
| Logging and assertions (#003) | 056 | 1 |
| Platform layer (Windows, Linux) (#004–#005) | 082 | 1 |
| Memory subsystem (#007) | 083-085 | 3 |
| Job system (#067) | 086-090 | 5 |
| Events and input (#008–#009) | 097, 108 | 2 |
| Renderer overview, Vulkan bring-up (#012–#021) | 121-129 | 9 |
| Renderbuffers, dynamic buffers (#044, #072) | 126, 127, 134 | 3 |
| Pipelines, shaders, buffers, first triangle (#024–#028) | 130-133, 147, 151 | 6 |
| Uniforms and descriptors (#029) | 130, 174 | 2 |
| Multiple render passes, render targets (#039, #059) | 141-145 | 5 |
| Meshes and transforms (#053–#054) | 147-149, 221, 222 | 5 |
| Camera, views, transparency, skybox (#060–#063) | 148, 155 | 2 |
| Lighting, normal maps (#049–#051) | 152-155 | 4 |
| Texture, material, geometry, resource systems (#035–#038) | 167-170, 181-187 | 11 |
| Game-code hot reload (Kohi feature) | 367-371 | 5 |
| Plugins (renderer, audio, UI) (Kohi structure) | 375-379 | 5 |

## Measures and enforcement points

- PLAN5-R02 and the table: `just registry-views-check` fails with R-VIEW-DRIFT when `registry/views/kohi-map.md` differs from the registry.
- The table: `just plan-check` requires it to equal the generated arc table line for line (R-PLAN-KOHI).
- PLAN5-R04: `just plan-check` fails when this document contains a code fence (R-PLAN-CLEAN-ROOM).
- PLAN5-R01, PLAN5-R03, PLAN5-R05 to PLAN5-R07: review by the founder; no recipe can see what was read or watched.
- The document: `just doc-check`.

## Traceability

Master plan sections 5, 5.1 and 5.2; SL-REG-002 `kohi_ref` of the Kohi-referenced episodes (for example
SL-EP-041 to SL-EP-048, SL-EP-056, SL-EP-082); SL-VOL-03-SPEC for the Kohi references of 041-048 and 056; work order WO-011.

## Open issues

- The "where Sliced Labs deliberately differs" column of section 5.1 is not in the registry, so the arc table omits it; section 5.1 stays the source.
- `clean_room` is a generated view value, not a registry field; promoting it to the registry needs a schema change outside WO-011.
- The clean-room rule is stated by work order WO-011; master plan section 5 only calls Kohi a build-along guide and does not use the term.
- Kohi episode numbers are approximate and unverified (master plan section 5.1).

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-011). |
