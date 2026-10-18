# docs/arch

Architecture references `SL-ARCH-001` to `SL-ARCH-030` (master plan, Required documents and registries). None is
written yet: all thirty are Planned in SL-REG-001 (`registry/SL-REG-001-documents.json`), and no `SL-ARCH-*.md` file
exists in this directory. The authoring-order waiver in
[ADR-0003](../adr/ADR-0003-authoring-order-waivers.md) lets the Volume 03 code of episodes 041-048 start before
SL-ARCH-001, SL-ARCH-002, SL-ARCH-003, SL-ARCH-005 and SL-ARCH-008 exist; it does not authorise skipping them for later
volumes. Until a document is written and registered with a path and a status above Planned, nothing here is normative.

Each document is written in the volume that first needs it (the volumes of its registered episodes). Titles and target
volumes are read from SL-REG-001 and SL-REG-002; the registry stays authoritative (master plan 6.3 rule 1).

| Id | Title | Target volume(s) | Status |
| :---- | :---- | :---- | :---- |
| SL-ARCH-001 | Canonical Project Model (Fabric) | 04 | Planned |
| SL-ARCH-002 | Transaction, Undo and Recovery Protocol | 04 | Planned |
| SL-ARCH-003 | Runtime and Execution Architecture | 05 | Planned |
| SL-ARCH-004 | Editor Shell and UI Framework | 06 | Planned |
| SL-ARCH-005 | Graphics Backend Contract (Vulkan, Metal, WebGPU, consoles) | 07, 26 | Planned |
| SL-ARCH-006 | Render Graph and Renderer | 08 | Planned |
| SL-ARCH-007 | Shader and Material System (Slang) | 09 | Planned |
| SL-ARCH-008 | Asset Database and Build Graph | 10 | Planned |
| SL-ARCH-009 | Content Production Domains (2D, geometry, animation, audio, UI, VFX) | 11, 12, 13, 14 | Planned |
| SL-ARCH-010 | Simulation, Physics and Networking | 15 | Planned |
| SL-ARCH-011 | Observability and Insights | 16 | Planned |
| SL-ARCH-012 | Determinism and Replay Contracts | 17 | Planned |
| SL-ARCH-013 | Collaboration and Revision Store | 18 | Planned |
| SL-ARCH-014 | Code Toolchain, Graph IR, Luau, Hot Reload | 19 | Planned |
| SL-ARCH-015 | AI Governance and Agent Tooling | 20, 42 | Planned |
| SL-ARCH-016 | Services and Identity | 23 | Planned |
| SL-ARCH-017 | SDK, Plugin ABI and Security Model | 24 | Planned |
| SL-ARCH-018 | Cluster Geometry, GI and Shadows | 27 | Planned |
| SL-ARCH-019 | Film-Quality Rendering and Color Pipeline | 28 | Planned |
| SL-ARCH-020 | Large Worlds and Procedural Generation | 29 | Planned |
| SL-ARCH-021 | Characters and Digital Humans | 30 | Planned |
| SL-ARCH-022 | Advanced Physics | 31 | Planned |
| SL-ARCH-023 | Gameplay Frameworks and Online Services | 32 | Planned |
| SL-ARCH-024 | Mobile, XR and Console Platform Layers | 33, 34, 35 | Planned |
| SL-ARCH-025 | Web Runtime, Web Studio and Stream | 36 | Planned |
| SL-ARCH-026 | Sliced Hub Web Platform | 37 | Planned |
| SL-ARCH-027 | Marketplace and Creator Runtime | 38 | Planned |
| SL-ARCH-028 | Interchange and Migration | 39 | Planned |
| SL-ARCH-029 | Virtual Production | 40 | Planned |
| SL-ARCH-030 | Enterprise Deployment | 41 | Planned |

`just docs` links this directory from `build/docs/index.md`; `just docs-check` verifies that every id cited here exists in
SL-REG-001. Work order: WO-031 (this index only).
