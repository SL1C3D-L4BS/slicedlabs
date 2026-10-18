---
id: ADR-0005
title: "CMake 3.28 floor, presets schema 8, import std as a probe"
status: Proposed
date: 2026-10-17
decision_refs: []
supersedes: []
---
# ADR-0005: CMake 3.28 floor, presets schema 8, import std as a probe

## Context

The starter prompt asks for CMake 3.28+ with Ninja and presets for debug, release, asan, ubsan and tsan (master plan,
Starter prompt, constraints). Locked decision: C++23 baseline with C++26 features promoted by toolchain feature
tests (master plan section 1.5, Engine language). Machine facts (plan of record, 2026-10-17): cmake 4.4.4, ninja 1.13.2,
clang 23.1.3 and gcc 16.2.1 are installed; `-std=c++26` compiles on both; there is no `<meta>` reflection;
`import std` is unverified.

## Decision

- The root CMake file MUST declare `cmake_minimum_required(VERSION 3.28)` (master plan, Starter prompt, constraints).
- Presets MUST use schema version 8 (plan of record: presets v8, min 3.28; WO-014 checks it).
- A feature newer than 3.28 MUST be guarded by `CMAKE_VERSION` (plan of record, Decisions table, ADR-0005 row); `CMAKE_CXX_SCAN_FOR_MODULES` is set to OFF.
- `import std` is a report-only probe and never fails configure (master plan section 1.5, C++26 promoted by feature tests).
- Only cmake 4.4.4 is tested (machine fact above); the 3.28 floor itself is an open issue.

## Consequences

- WO-014 and WO-015 implement the presets, the C++23 required list and the C++26 probe list.
- The 3.28 floor is untested and is recorded as an open issue in the 041-048 episode notes.

## Alternatives

- Floor at 4.4.4: rejected, the starter prompt says 3.28+ (master plan, Starter prompt).
- Enable module scanning and `import std` now: rejected, unverified on both compilers (machine fact above).
