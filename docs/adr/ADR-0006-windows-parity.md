---
id: ADR-0006
title: "Windows parity is verified structurally on Linux"
status: Accepted
date: 2026-10-17
decision_refs: []
supersedes: []
---
# ADR-0006: Windows parity is verified structurally on Linux

## Context

The starter prompt requires that Windows (MSVC + Clang-cl) must also configure (master plan, Starter prompt,
constraints). The graphics policy puts Windows on Vulkan (master plan section 2.6). The workstation is Arch Linux and
has clang-cl 23 but no Windows host (machine fact, plan of record, 2026-10-17). The release gates require evidence
on every platform (master plan, Release gates).

## Decision

- On Linux, Windows parity MUST be verified only structurally: a presets check, `cmake -P` tests of the compiler flag table with mocked MSVC and clang-cl ids, a clang-cl flag-acceptance probe and a portability lint (plan of record, Decisions table, ADR-0006 row).
- These checks are not a Windows configure and MUST NOT be reported as one (master plan, Release gates: evidence on every platform).
- Real configure evidence SHOULD come from a `workflow_dispatch` windows-latest job run after the first push by the founder (plan of record, Decisions table).
- `.github/**` is a governed path, so changes to it need the founder (master plan section 6.3 rule 4; plan of record, FACTORY.md governed list).
- Until that job has run, the 041-048 episode notes MUST list Windows as unverified (master plan section 6.1, completion record).

### Amendment 1 (Proposed by WO-131; in force when the founder accepts it)

- Windows-only change: the `windows` job installs one pinned LunarG Vulkan SDK (version and sha256 in `windows.yml`, sha256 checked before the installer runs, cached by version and sha256, no third-party action) and exports `VULKAN_SDK`. windows-latest has no SDK: run 40063791699 failed `find_package(Vulkan REQUIRED)` (`cmake/SlVulkan.cmake:78`) on both presets. The pin (1.4.363.0, URL and sha256) was founder-fetched on 2026-10-24 (`docs/ops/ci.md`); the installer command line has not yet run on a runner. The job still configures only; the Linux gate is unchanged.
- Known Windows gaps. Seen in the configure logs of run 40063791699: windows-latest, Visual Studio 18.11.1, MSVC toolset 14.51.36231 (cl 19.51.36260.0), clang-cl 20.1.8, both on the MSVC STL. Upstream status was not checked (no network access when written); trackers: [MSVC STL changelog](https://github.com/microsoft/STL/wiki/Changelog), [cppreference compiler support](https://en.cppreference.com/w/cpp/compiler_support).

| Feature | Toolset seen | Result | Effect |
| :---- | :---- | :---- | :---- |
| Required C++23 battery (flat_map, flat_set, mdspan, ranges::to, all 21) | cl 19.51, clang-cl 20.1.8 | 21/21 ok on both | no gap, presets unchanged |
| `std::stdfloat` (deferred list) | both | `__cpp_lib_stdfloat` missing; no `std::float32_t` | never fatal (`deferred-cxx23.txt`) |
| C++26 probes (report only) | cl 19.51, clang-cl 20.1.8 | 1/13 and 5/13 ok; both lack reflection, contracts, inplace_vector, simd, saturating, function_ref, ranges_concat | never fatal |
| Validation layer manifest | SDK `Bin` | `SL_VK_LAYER_DIRS` has no Windows default (`SlVulkan.cmake:17`), so validation is 0 with a warning | needs a `cmake/` change, outside WO-131 |
| Configure after `sl_vulkan_detect` (deps, build info, targets) | both | never reached on Windows | unknown until the next hosted run |

## Consequences

- WO-014 builds the structural checks; WO-020 adds the CI scripts; WO-021 records the open issue.
- No claim of Windows support is made before the hosted job passes.

## Alternatives

- A local Windows VM: rejected for this slice, the plan takes hosted evidence after the first push.
- Skip Windows until later: rejected, the starter prompt requires it to configure (master plan, Starter prompt).
