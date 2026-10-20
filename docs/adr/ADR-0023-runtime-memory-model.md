---
id: ADR-0023
title: "Runtime memory model"
status: Proposed
date: 2026-10-20
decision_refs: []
supersedes: []
---
# ADR-0023: Runtime memory model

## Context

Episodes 083-085 ask for allocation contracts (alignment, lifetime, ownership, instrumentation), foundational
allocators and tagged diagnostics feeding the profiler (master plan, Volume 05, episodes 083-085; section 5.1, row
Memory subsystem). `std::pmr::memory_resource::do_allocate` is specified to throw on failure while errors are values
(ADR-0009). Some memory the runtime holds is allocated elsewhere: a `document_projection` copies its records in
`make()` through global new (machine fact: engine/fabric/include/sl/fabric/boundary.hpp), and Volume 07 will hold GPU
heaps (master plan, Volume 07).

## Decision

- Every runtime allocator MUST derive from `mem::resource`, a std::pmr::memory_resource with a noexcept `try_allocate(bytes, align, site)` returning `sl::result<void*>`; runtime code on fallible paths MUST call try_allocate (ADR-0009).
- `do_allocate` MUST forward to a configured upstream or, with none, SL_ASSERT-abort with a tag report; it MUST NOT throw across runtime code (ADR-0009).
- Alignment MUST be a power of two up to 4096; every tracked allocation MUST carry a memory tag from a registry frozen at runtime start, with gpu.* and fabric.* reserved for external accounting (master plan section 5.1, row Memory subsystem).
- Recycling allocators MUST fill memory in Debug (0xCD fresh, 0xDD freed) and poison freed and reset ranges under ASan (master plan, Volume 05, episode 083).
- Memory the runtime does not allocate MUST be reported through `report_external(tag, bytes)` and never counted as tracked live bytes (master plan section 3, F6).
- Volume 05 MUST NOT replace global operator new or delete (master plan section 6.1; Volume 16 owns process-wide tracking).
- After a 10-frame warm-up a steady frame MUST make zero upstream allocations, the work-bound performance case of episode 084 (master plan section 6.1).

## Consequences

- The build keeps exceptions enabled; the nothrow lint (WO-077) bans throw, try and catch in engine/runtime.
- The allocator contract suite (WO-079) runs against all six allocators (WO-081).
- Memory reports are sl-runtime-memreport/1 (schemas/runtime/memreport.schema.json, WO-075).

## Alternatives

- Catch `std::bad_alloc` at the world boundary: rejected, it half-isolates faults (writes already made stay).
- A custom allocator interface without pmr: rejected, std containers could not use it.
- Replace global new for tracking: rejected, process-global mutation breaks two runtimes per process.
