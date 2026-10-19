---
id: ADR-0021
title: "Volume 02 deferral: Volume 05 before Volume 02, Volume 02 before Volume 07, and the mathematics allowed meanwhile"
status: Accepted
date: 2026-10-19
decision_refs: []
supersedes: []
---
# ADR-0021: Volume 02 deferral: Volume 05 before Volume 02, Volume 02 before Volume 07, and the mathematics allowed meanwhile

## Context

Volume 05 (episodes 081-100, runtime, memory, jobs, ECS) depends on Volumes 02 and 04, and Volume 07 (episodes
121-140, Vulkan) depends on Volume 05 (master plan section 4.2; machine fact: `depends_on_volumes` of SL-VOL-05 is
`[SL-VOL-02, SL-VOL-04]` and of SL-VOL-07 is `[SL-VOL-05]` in `registry/SL-REG-002-episodes.json`). Volume 02
(episodes 021-040, mathematics and computer-science foundations) is unbuilt. Every episode has `depends_on: []`
(machine fact: `registry/SL-REG-002-episodes.json`), so `check_dependencies` in `cmd_ep_advance.py` enforces nothing
between volumes today (machine fact). An episode is accepted only after its dependencies are accepted (master plan
section 6.2). ADR-0003 waives that rule for 041-048 and ADR-0020 caps 061-080 at Demonstrated; neither covers Volume 05
or Volume 07.

## Decision

- Episodes 081-100 MAY start (InProgress) and reach Demonstrated before Volume 02 is Accepted (master plan section 4.2 is waived for this pair, as ADR-0003 and ADR-0020 do for theirs).
- Episodes 081-100 MUST NOT reach Accepted before episodes 021-040 are Accepted (master plan section 6.2).
- No Volume 07 episode (121-140) MAY enter InProgress before every episode 021-040 is Accepted (master plan section 4.2).
- Until then Volume 05 code MAY use only the mathematics C++23 and the standard library already provide: `<cmath>`, `<numbers>`, `<numeric>` (including `std::midpoint` and `std::lerp`), `<bit>`, `<random>`, `<chrono>`, `<ratio>`, `<limits>`, `<cstdint>`, `<algorithm>` and `<ranges>`, `<complex>` and `std::hash` (master plan section 4.2).
- Volume 05 code MUST NOT define or vendor vector, matrix, quaternion, transform, SIMD, fixed-point, curve or geometry types; those belong to Volume 02, episodes 021-023 and 034-036 (master plan section 4.2).
- The ADR-0020 cap (episodes 061-080 reach Accepted only after 041-060 are Accepted) MUST be enforced by the same mechanism (ADR-0020).
- The registry lifecycle MUST carry these as `lifecycle.gates` plus one `R-DEP-ACCEPTED` waiver for 081-100 with `max_status` Demonstrated, and no Volume 05 or Volume 07 work order MAY edit episode `depends_on` (master plan section 6.3 rule 3; the founder owns registry amendments).

## Consequences

- `lifecycle.gates` in the registry lists three gates: ADR-0020 (061-080 Accepted needs 041-060 Accepted), ADR-0021
  (081-100 Accepted needs 021-040 Accepted) and ADR-0021 (121-140 InProgress needs 021-040 Accepted); the generator
  `cmd_gen_episodes.py` owns the list (machine fact) and the lock digest does not change.
- `ep-advance` refuses with `R-VOL-GATE`, naming the ADR and the first unmet episode; `--adr` does not bypass it.
- `registry-check` compares the gates and waivers with the generator, reports `R-VOL-GATE` for an episode at or beyond
  a gated state, and has planted-defect mutations for it (WO-072).
- SL-ARCH-003 for Volume 05 cites this ADR; Volume 05 reviews reject any math type outside the allow-list.
- The founder accepts this ADR through `scripts/founder/approve-001-080.sh`; a builder never does.

## Alternatives

- Build Volume 02 first: rejected, the founder ordered the runtime first and the allow-list needs no new code.
- Let Volume 05 write its own vector and matrix types: rejected, it would fork Volume 02's types (master plan section 4.2).
- Edit episode `depends_on` instead of gates: rejected, those are founder-owned registry amendments (master plan section 6.3 rule 3).
