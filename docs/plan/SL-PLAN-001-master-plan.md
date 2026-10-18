# **Sliced Labs — Unified Engine & Production Platform: Official Master Plan**

Oct 17, 2026 · @Michael

## **1\. Product definition and doctrine**

Sliced Labs is one unified interactive production platform: a native C++23/Vulkan 2D/3D engine and editor in which every authoritative project fact has one identity, one history, one dependency graph and one transaction model. This is the only official version of the plan. It carries 840 episodes in 42 volumes across 7 parts, with stable IDs SL-EP-001 to SL-EP-840 and SL-VOL-01 to SL-VOL-42. Every capability Unreal Engine ships has a Sliced Labs counterpart decided and specified here, plus a full-stack web platform; nothing is deferred to an unnamed later version.

Companion tabs: Competitive research (Unreal, Unity, Godot, Kohi and the capability superset), Detailed architecture (data models, runtime, rendering, web platform), UI/UX specification (design system, project modes, every workspace).

The build follows the arc of the Kohi Game Engine series, but in C++23 and with Sliced Labs' own architecture. Kohi teaches how an engine is put together; this plan says what Sliced Labs adds on top.

### **1.1 One-sentence definition**

Sliced Labs is a unified interactive production platform in which every authoritative project fact has one identity, one history, one dependency graph, one transaction model and one live execution environment, with measured, governed boundaries where external toolchains, platform SDKs or specialist dependencies are unavoidable.

### **1.2 Three founding principles**

1. **One canonical project model.** Every asset, entity, system, dependency, build configuration, test result and runtime observation has one authoritative identity and a traceable history.  
2. **One continuous production environment.** Authoring, simulation, debugging, profiling, collaboration, testing, packaging and release share one interaction model, one command system, one transaction protocol and one dependency graph.  
3. **Multiple execution environments, one platform.** Editor, runtime, headless test runner, dedicated server, build worker and collaboration service may run in separate processes or machines, but share schemas, execution semantics, artifact identities and evidence contracts.

### **1.3 Doctrine: zero manual fragmentation**

The promise is not "no external program ever runs." It is four measurable zeros:

* Zero manual export/import steps in native-owned workflows.  
* Zero competing authoritative representations of the same fact.  
* Zero untracked source-to-derived transformations.  
* Zero stale artifacts presented as current.

Compilers, debuggers, GPU drivers, platform SDKs, signing tools, middleware behind adapters, remote workers and interchange formats (glTF, USD, FBX, OpenEXR, MaterialX) are allowed. They become fragmentation only when they create a second source of truth, an untracked transformation, or a manual reconciliation step.

### **1.4 Release scope and delivery order**

| Milestone | Episodes | What ships |
| :---- | :---- | :---- |
| Foundation | 001–100 | Canonical model, toolchain, runtime, ECS |
| Core loop | 101–200 | Editor, Vulkan renderer, materials, asset pipeline |
| Production | 201–400 | 2D, 3D, animation, audio, VFX, physics, networking, profiling, replay, collaboration, code, governed AI |
| Early Access | 401–500 | Verification, build farm, services, SDK, Linux \+ Windows desktop release |
| Sliced Labs 1.0 | 501–840 | Unreal-class rendering, large worlds, digital humans, destruction, all platforms, web runtime, Sliced Hub website, marketplace, virtual production, enterprise |

All of it is in the 1.0 scope. The order is a dependency order, not a deferral: each milestone ships to real users so the next one is built on evidence.

Specifying everything now is done in this document. Building all of it to Unreal's quality is a large-team, multi-year program; the plan is written so each episode is still buildable by one person with Claude Code, in order.

### **1.5 Locked, conditional and rejected decisions**

| Area | Decision (locked) |
| :---- | :---- |
| Engine language | C++23 baseline; C++26 features promoted by toolchain feature tests |
| Build system | CMake \+ Ninja for native code; Sliced build graph for content and cooking |
| Graphics | Vulkan 1.3+ (Windows, Linux, Android); Metal 3 native (macOS, iOS, visionOS); WebGPU (browser); console APIs behind the same backend contract |
| Shader language | Slang, compiling to SPIR-V, Metal, WGSL and console targets |
| Runtime model | Archetype ECS; job system with declared read/write access |
| Extension language | Luau (typed, sandboxable, fast) for tools, mods and gameplay iteration; native C++ for engine and shipping hot paths |
| Visual scripting | Typed graph IR lowered to C++ for shipping and Luau bytecode for live iteration |
| UI | Own retained-mode UI framework for editor and games; immediate-mode debug layer on top of it |
| Physics | Jolt Physics behind an adapter for rigid bodies; native destruction, cloth, fluids and vehicles (Vol 31\) |
| Audio | Native mixer and DSP graph; native procedural audio graph |
| Networking | Native replication over a reliable-UDP transport; QUIC for services |
| Version control | Native content-addressed revision store; Git interop for source code |
| Web front end | TypeScript, React, Next.js |
| Web services | Rust (axum), PostgreSQL, S3-compatible object storage, NATS JetStream, Kubernetes, OpenTelemetry |
| Browser runtime | Engine compiled to WebAssembly with the WebGPU backend |
| AI | Provider-neutral model adapters; local and hosted; every edit through governed transactions |
| Hot reload | Formal protocol only (section 2.5); arbitrary C++ reload rejected |
| Process model | One logical platform, multiple processes; single-process-for-everything rejected |
| Determinism | Contract per subsystem, proven by tests (Vol 17\) |

## **2\. Platform architecture**

Twelve logical subsystems sit on one canonical core (the Fabric); seven execution environments share its contracts. Subsystems are boundaries in the code, not separate programs or separate sources of truth.

### **2.1 Layered architecture**

flowchart TB  
  subgraph USERS\[Actors\]  
    DEV\[Developer\]:::a  
    ART\[Artist\]:::a  
    AGT\[AI agent\]:::a  
  end  
  subgraph STUDIO\[Sliced Studio \- native production environment\]  
    SHELL\[01 Editor shell\<br/\>workspaces, commands, docking\]  
    CONTENT\[05 Content workspaces\<br/\>2D, 3D, materials, animation, audio, VFX, UI\]  
    CODE\[07 Code and graph toolchain\]  
    OBS\[09 Observability and replay\]  
  end  
  subgraph FABRIC\[02 Canonical project model \- the Fabric\]  
    ID\[Identity and schemas\]  
    TX\[Transactions and undo\]  
    DEP\[Dependency graph and change impact\]  
    PROV\[Provenance and evidence\]  
    GOV\[12 Permissions and AI governance\]  
  end  
  subgraph EXEC\[Execution\]  
    RT\[03 Runtime: ECS, jobs, memory, clocks\]  
    GFX\[04 Graphics: Vulkan, render graph, compute\]  
    SIM\[06 Simulation: physics, AI, networking\]  
    ASSET\[08 Asset and build graph: CAS, cook, stream\]  
  end  
  subgraph SHIP\[Delivery\]  
    COLLAB\[10 Collaboration and history\]  
    VER\[11 Verification and delivery\]  
  end  
  USERS \--\> SHELL  
  SHELL \--\> CONTENT & CODE & OBS  
  CONTENT & CODE \--\> TX  
  AGT \--\> GOV \--\> TX  
  TX \--\> ID & DEP & PROV  
  DEP \--\> ASSET  
  ID \--\> RT  
  RT \--\> GFX & SIM  
  ASSET \--\> RT  
  RT \--\> OBS  
  TX \--\> COLLAB  
  ASSET \--\> VER  
  classDef a fill:\#eef,stroke:\#88a  
Every write from a person, tool, plugin or agent enters through the transaction layer; nothing in Execution or Delivery mutates authoritative data directly.

### **2.2 The twelve subsystems**

| \# | Subsystem | Owns (authoritative) | Derives / consumes |
| :---- | :---- | :---- | :---- |
| 01 | Unified Editor Shell | Workspace layouts, command registry, selection | Views over every other subsystem |
| 02 | Canonical Project Model (Fabric) | IDs, schemas, revisions, transactions, provenance | — |
| 03 | Runtime Core | Live simulation state (transient) | Component schemas from 02 |
| 04 | Graphics and Compute | GPU resources, pipelines (derived) | Materials, meshes, render settings |
| 05 | Content Production | Editable source docs: meshes, sprites, graphs, curves, audio edits | — |
| 06 | Simulation and Gameplay | Gameplay config, nav settings, net topology | Physics/nav data as derived artifacts |
| 07 | Code and Graph Toolchain | Source files, graph IR | Compiled modules, debug symbols |
| 08 | Asset and Build Graph | Content-addressed store, build DAG | Cooked artifacts with provenance |
| 09 | Observability and Replay | Traces, captures, checkpoints | Correlation to IDs/ticks/revisions |
| 10 | Collaboration and History | Accepted revisions, branches, locks | Semantic diffs |
| 11 | Verification and Delivery | Test results, release manifests | Packages, signatures |
| 12 | Governed Automation | Agent permissions, approvals, audit log | Proposed change sets |

### **2.3 Execution environments**

flowchart LR  
  subgraph LOCAL\[Workstation\]  
    ED\[Editor process\]  
    PRE\[Runtime preview\<br/\>in-process or sandbox\]  
    HL\[Headless test runner\]  
  end  
  subgraph REMOTE\[Optional remote\]  
    BW\[Build workers\]  
    CS\[Collaboration service\]  
    AI\[AI inference\<br/\>local or hosted\]  
  end  
  SHIPPED\[Shipped runtime\<br/\>player or dedicated server\]  
  CONTRACT\[(Shared contracts:\<br/\>IDs, schemas, event envelopes,\<br/\>evidence records)\]  
  ED \<--\> PRE  
  ED \--\> HL  
  ED \<--\> CS  
  ED \--\> BW  
  ED \<--\> AI  
  BW \--\> SHIPPED  
  ED \-.-\> CONTRACT  
  PRE \-.-\> CONTRACT  
  HL \-.-\> CONTRACT  
  BW \-.-\> CONTRACT  
  CS \-.-\> CONTRACT  
  SHIPPED \-.-\> CONTRACT  
Local authoring, preview and core builds must work with no remote service running. Remote pieces add scale, never authority.

### **2.4 Transaction lifecycle**

sequenceDiagram  
  actor U as User or agent  
  participant S as Studio / tool  
  participant T as Fabric transaction  
  participant V as Schema \+ policy validator  
  participant R as Revision store \+ CAS  
  participant D as Dependency graph  
  participant E as Tests \+ evidence  
  U-\>\>S: Request change  
  S-\>\>T: Typed patch \+ base revision  
  T-\>\>V: Validate schema, authority, invariants  
  alt rejected  
    V--\>\>S: Structured rejection, nothing written  
  else accepted  
    V--\>\>T: Pass  
    T-\>\>R: Commit atomically  
    R--\>\>T: New revision \+ event ID  
    T-\>\>D: Compute change-impact closure  
    D--\>\>S: Affected assets, views, builds  
    D-\>\>E: Schedule affected tests  
    E--\>\>S: Evidence \+ status  
  end

### **2.5 Live-update and hot-reload protocol**

stateDiagram-v2  
  \[\*\] \--\> Compile: change detected  
  Compile \--\> Validate: candidate built  
  Compile \--\> Rejected: build error  
  Validate \--\> AwaitBoundary: ABI, schema, migration OK  
  Validate \--\> RestartRequired: layout or core ABI changed  
  AwaitBoundary \--\> Swap: safe frame/tick boundary  
  Swap \--\> HealthCheck  
  HealthCheck \--\> Committed: checks pass  
  HealthCheck \--\> RolledBack: checks fail  
  RolledBack \--\> \[\*\]  
  Committed \--\> \[\*\]  
  Rejected \--\> \[\*\]  
  RestartRequired \--\> \[\*\]

| Change class | Behavior |
| :---- | :---- |
| Data, materials, compatible resources | Applied live at a safe boundary |
| Gameplay functions with stable interfaces | Hot reload where supported |
| Component schema / object layout | Validated migration or controlled restart |
| Allocator, scheduler, runtime ABI | Isolated dev runtime or restart |
| Platform or toolchain | Rebuild the target environment |

### **2.6 Graphics backend policy**

| Platform | Backend | Volume |
| :---- | :---- | :---- |
| Linux | Vulkan | 07 |
| Windows | Vulkan (DirectX 12 adapter for Xbox shared code) | 07, 35 |
| macOS | Metal 3 native | 26 |
| iOS, iPadOS | Metal 3 native | 33 |
| Android | Vulkan | 33 |
| Meta Quest, Android XR | Vulkan \+ OpenXR | 34 |
| Apple Vision Pro | Metal \+ visionOS | 34 |
| PlayStation, Xbox, Nintendo Switch 2 | Platform APIs behind the backend contract (NDA SDKs) | 35 |
| Browser | WebGPU via WebAssembly | 36 |

Mesh shaders, ray tracing and bindless are capability-gated paths; the profiler always shows which path ran.

### **2.7 Unreal parity matrix**

Every major Unreal Engine 5 system has a named Sliced Labs counterpart, an owning volume, and the measurable way Sliced Labs must beat it. "Better" is only claimed when the Volume 41 benchmark gate (SL-BENCH reference scenes, same hardware, same visual target) passes.

| Unreal system | Sliced Labs counterpart | Volume | Where Sliced Labs must be better |
| :---- | :---- | :---- | :---- |
| Nanite virtualized geometry | Sliced Cluster Geometry (cluster LOD DAG, GPU-driven, streaming) | 27 | Source mesh stays editable; no separate build step; LOD error visible per cluster |
| Lumen dynamic GI and reflections | Sliced Radiance (software \+ hardware RT GI, surface cache) | 27 | Every GI input traceable; quality tier shown per view; no bake step |
| Virtual Shadow Maps | Sliced Virtual Shadows | 27 | Page-cache cost shown per light in the profiler |
| Temporal Super Resolution | Sliced Temporal Upscaler \+ vendor upscaler adapters | 28 | Upscaler choice is a capability-gated, benchmarked setting |
| Substrate materials | Sliced Layered Materials (graph-native, slab model) | 09, 28 | One material graph for 2D, 3D, mobile and web with automatic tier lowering |
| Path tracer, Movie Render Queue | Sliced Path Tracer \+ Render Queue | 28, 40 | Final frames carry full provenance and are reproducible from a revision |
| Landscape, Water | Sliced Terrain \+ Water Bodies | 29 | Terrain edits transactional and mergeable between users |
| World Partition, One File Per Actor | Sliced World Streaming on canonical entity IDs | 29, 18 | Semantic merges per entity, not per file |
| PCG framework | Sliced Procedural Graphs | 29 | Seeded, deterministic, diffable output with provenance |
| Niagara | Sliced VFX Graph (CPU \+ GPU) | 14 | Same graph IR as materials and gameplay; deterministic mode for replay |
| Chaos physics, destruction, cloth | Jolt adapter \+ native destruction, cloth, fluids, vehicles | 15, 31 | Replay-safe determinism contract per solver |
| Groom hair | Sliced Strands | 30 | Authored in-engine; no external groom tool needed |
| MetaHuman | Sliced Digital Humans (scan import, face rig, LOD, customization) | 30 | Full pipeline inside the editor, editable at every stage |
| Control Rig, IK Retargeter | Sliced Rig Graph \+ Retargeter | 13, 30 | Rig graph shares the typed graph IR |
| Motion Matching, Pose Search | Sliced Motion Matching | 30 | Database rebuild is incremental via the build graph |
| Mutable character customization | Sliced Customization Graphs | 30 | Runtime variants are derived artifacts with provenance |
| Sequencer | Sliced Sequencer | 13 | Same animation data for gameplay and cinematics |
| Blueprints | Sliced Graphs (typed IR → C++ / Luau) | 19 | Graphs compile to native for shipping; diffable and mergeable |
| Verse / UEFN | Luau extension layer \+ Sliced creator runtime | 19, 38 | Sandboxed creator content with permissioned APIs |
| UMG / Slate | Sliced UI (one retained framework) | 06, 14 | Editor and game UI on one framework |
| MetaSounds | Sliced Sound Graphs | 14, 32 | Shares graph IR; sample-accurate replay |
| Mass Entity | Native archetype ECS (the core runtime, not an add-on) | 05, 32 | Crowds use the same ECS as everything else |
| Gameplay Ability System | Sliced Abilities | 32 | Ability data is schema-native and replay-safe |
| Iris replication, Online Subsystem, EOS | Sliced Replication \+ Sliced Online Services | 15, 32 | Divergence detection built in; services run on Sliced Hub |
| Live Coding | Hot-reload protocol with ABI manifest and rollback | 19 | Rollback instead of crash on incompatible change |
| Unreal Insights | Sliced Insights (CPU, GPU, memory, net, assets) | 16 | Every sample tied to entity, system, revision and tick (F6) |
| Multi-User Editing, Concert | Native collaboration on the Fabric | 18 | Semantic three-way merges, offline reconciliation |
| Horde, Zen storage | Sliced Build Farm \+ CAS | 10, 22 | Reproducible artifacts with provenance (F3) |
| Datasmith, USD | Sliced Interchange (USD, glTF, FBX, Alembic, MaterialX) | 39 | Imports keep source identity and fidelity reports |
| Live Link | Sliced Live Link (mocap, face capture, DCC sync) | 39, 40 | Recorded takes become versioned assets |
| nDisplay, virtual production | Sliced Stage (LED walls, camera tracking, genlock) | 40 | Stage config is part of the project model |
| Pixel Streaming | Sliced Stream (WebRTC cloud rendering) | 36 | Shares the web runtime and Hub accounts |
| Fab marketplace, Quixel library | Sliced Marketplace \+ asset library | 38 | Packages carry provenance and compatibility proofs |
| Lyra sample game | Sliced reference games (2D, 3D, multiplayer, VR) | 25, 41 | Each is also a benchmark scene |

### **2.8 Full-stack web platform**

Sliced Labs is also a website and cloud platform: Sliced Hub. It is optional for local work, required for marketplace, cloud builds, web studio and online services.

flowchart TB  
  subgraph CLIENTS\[Clients\]  
    DESK\[Sliced Studio\<br/\>desktop editor\]  
    WEB\[Sliced Web Studio\<br/\>browser, WASM \+ WebGPU\]  
    SITE\[Sliced Hub website\<br/\>Next.js\]  
    GAME\[Shipped games\<br/\>all platforms\]  
  end  
  subgraph EDGE\[Edge\]  
    GW\[API gateway\<br/\>auth, rate limits\]  
    RTC\[WebRTC signaling\<br/\>Sliced Stream\]  
  end  
  subgraph SVC\[Rust services\]  
    IDS\[Identity \+ orgs\]  
    PROJ\[Projects \+ collaboration\]  
    BUILD\[Cloud build orchestrator\]  
    MKT\[Marketplace \+ payments\]  
    ONL\[Online services:\<br/\>lobbies, matchmaking,\<br/\>leaderboards, saves\]  
    DOCS\[Docs \+ learn\]  
    TEL\[Telemetry \+ crash reports\]  
  end  
  subgraph DATA\[Data\]  
    PG\[(PostgreSQL)\]  
    OBJ\[(Object storage\<br/\>CAS blobs)\]  
    BUS\[(NATS JetStream)\]  
  end  
  WORK\[GPU build and render workers\<br/\>Kubernetes\]  
  DESK & WEB & SITE & GAME \--\> GW  
  WEB \--\> RTC  
  GW \--\> IDS & PROJ & BUILD & MKT & ONL & DOCS & TEL  
  SVC \--\> PG & OBJ & BUS  
  BUILD \--\> WORK  
  RTC \--\> WORK

| Web product | What it does | Volume |
| :---- | :---- | :---- |
| Sliced Hub website | Accounts, organizations, projects, billing, downloads, status | 23, 37 |
| Sliced Web Studio | Open any project in a browser: view, review, comment, light editing, play builds | 36 |
| Sliced Stream | Cloud-rendered full editor or game over WebRTC for any device | 36 |
| Cloud Builds | Remote build, cook, test and package for every platform | 22, 37 |
| Sliced Marketplace | Assets, plugins, templates, creator payouts | 38 |
| Online Services | Lobbies, matchmaking, leaderboards, cloud saves, voice for shipped games | 32 |
| Learn | Docs portal, the build-along series, courses | 25, 37 |
| Community | Forums, showcases, issue tracker | 37 |
| Admin console | Org admin, audit log, SSO, compliance exports | 41 |

## **3\. Fragmentation budget**

Eight invariants turn "zero manual fragmentation" into CI checks. Each has a numerator, a denominator, a threshold and a named enforcement point. A release passes only when every applicable invariant meets its threshold.

| ID | Invariant | Measurement (numerator / denominator) | Threshold | Enforcement point | Episodes |
| :---- | :---- | :---- | :---- | :---- | :---- |
| F1 | No manual export/import in native workflows | Workflow-test steps requiring an external app / total steps, per discipline acceptance workflow | 0 | Workflow acceptance suite (Vol 21\) \+ build graph rejects undeclared inputs | 003, 200, 220, 408 |
| F2 | No duplicate authoritative representations | Data domains with \>1 registered owner / all domains | 0 | Identity registry rejects a second ownership claim | 066, 079 |
| F3 | Complete provenance | Derived artifacts missing any of: input IDs, input revisions, toolchain ID, parameters, environment hash / all derived artifacts | 0 | Artifact store refuses writes without the provenance record | 055, 182, 434 |
| F4 | No stale artifact shown as current | Artifacts served while an input revision changed or is unverifiable / artifacts served | 0 | Currency check on every artifact read | 187, 200 |
| F5 | Every edit is transactional | Mutations bypassing the transaction manager / all mutations (instrumented in debug builds) | 0 | Mutation APIs only reachable through the transaction manager | 070, 116, 390 |
| F6 | Observations are traceable | Trace events missing correlation fields / all events; process-wide events carry a scope tag instead of an entity | 0 missing required fields | Trace schema validator | 301, 319 |
| F7 | Minimal rebuild scope | Rebuilt nodes / dependency-closure nodes on the reference workload set | ≤ 1.05 (baseline set in ep. 198\) | Build graph report | 186, 198 |
| F8 | No external tool needed for final content in core disciplines | Core disciplines whose acceptance workflow needs an external app / core disciplines | 0 at 1.0 for every core discipline | Discipline acceptance suites | 220, 240, 260, 280 |

### **3.1 Declared exemptions**

Each exemption is registered in SL-REG-006 (Exception Register), shown in the editor, and reviewed every release. Allowed classes: console certification, platform signing, specialist simulation solvers, legal or regulatory validation, interchange import/export, and AI model training.

### **3.2 How a violation is handled**

flowchart LR  
  M\[Metric computed\<br/\>in CI or editor\] \--\> C{Over threshold?}  
  C \-- no \--\> OK\[Scorecard green\]  
  C \-- yes \--\> X{Registered exemption?}  
  X \-- yes \--\> W\[Shown as declared\<br/\>exception\]  
  X \-- no \--\> P{Phase}  
  P \-- authoring \--\> WARN\[Editor warning \+\<br/\>diagnostic record\]  
  P \-- local validation \--\> FAIL\[Configurable fail\]  
  P \-- release \--\> BLOCK\[Release blocked\]

### **3.3 Scorecard surfaced in the editor**

* Native-authored content coverage per discipline.  
* Provenance completeness (F3).  
* Over-invalidation ratio (F7).  
* Stale-artifact incidents (F4).  
* Transaction coverage (F5).  
* Cross-process boundary count and latency cost.  
* External-tool dependency surface, with each exemption listed.

## **4\. Program map**

The program is 840 episodes in 42 volumes of 20, grouped into 7 parts. Parts I–V (001–500) build the engine and ship Early Access; Parts VI–VII (501–840) bring Unreal-class depth, every platform, the web platform and the ecosystem, and ship 1.0. Numbering is a dependency order, not a calendar.

### **4.1 Volumes**

| Part | Volume | Episodes | Title |
| :---- | :---- | :---- | :---- |
| I Foundations | 01 | 001–020 | Product vision, requirements, competitive architecture |
| I | 02 | 021–040 | Mathematics, computer science, graphics foundations |
| I | 03 | 041–060 | Toolchain, repository, build infrastructure |
| I | 04 | 061–080 | Core architecture, identity, project data (Fabric) |
| I | 05 | 081–100 | Native runtime, memory, jobs, ECS |
| II Visible engine | 06 | 101–120 | Native editor shell and UI framework |
| II | 07 | 121–140 | Vulkan device, resources, synchronization |
| II | 08 | 141–160 | Render graph and core 3D rendering |
| II | 09 | 161–180 | Shaders, materials, textures, advanced rendering |
| II | 10 | 181–200 | Asset database, import, cooking, streaming |
| III Content and games | 11 | 201–220 | Native 2D studio |
| III | 12 | 221–240 | Geometry, modeling, sculpting, mesh authoring |
| III | 13 | 241–260 | Animation, cinematics, sequencing |
| III | 14 | 261–280 | Audio, UI, VFX, world authoring |
| III | 15 | 281–300 | Physics, gameplay, AI, networking |
| IV Intelligence | 16 | 301–320 | Debugging, profiling, observability (Sliced Insights) |
| IV | 17 | 321–340 | Deterministic simulation and time travel |
| IV | 18 | 341–360 | Source control, collaboration, history |
| IV | 19 | 361–380 | Native code toolchain, graphs, Luau, plugins |
| IV | 20 | 381–400 | AI-native editor and governed automation |
| V Early Access | 21 | 401–420 | Testing, verification, quality engineering |
| V | 22 | 421–440 | Build farm, packaging, deployment, delivery |
| V | 23 | 441–460 | Backend services, identity, operations |
| V | 24 | 461–480 | SDK, plugins, security, compatibility |
| V | 25 | 481–500 | Documentation, reference games, Early Access launch |
| VI Unreal-class depth | 26 | 501–520 | macOS Metal backend, portability, hardware lab |
| VI | 27 | 521–540 | Cluster geometry, dynamic GI, virtual shadows |
| VI | 28 | 541–560 | Film-quality rendering: path tracing, upscaling, layered materials, atmosphere |
| VI | 29 | 561–580 | Terrain, water, large worlds, procedural generation |
| VI | 30 | 581–600 | Characters and digital humans |
| VI | 31 | 601–620 | Destruction, cloth, fluids, vehicles |
| VI | 32 | 621–640 | Gameplay at scale: abilities, crowds, sound graphs, online services |
| VII Platforms and ecosystem | 33 | 641–660 | Mobile: Android and iOS |
| VII | 34 | 661–680 | XR: OpenXR, Quest, Android XR, Vision Pro |
| VII | 35 | 681–700 | Consoles: PlayStation, Xbox, Nintendo |
| VII | 36 | 701–720 | Web runtime, Web Studio, Sliced Stream |
| VII | 37 | 721–740 | Sliced Hub full-stack website |
| VII | 38 | 741–760 | Marketplace, asset library, creator economy |
| VII | 39 | 761–780 | Interchange, migration from other engines, Live Link |
| VII | 40 | 781–800 | Virtual production, broadcast, film output |
| VII | 41 | 801–820 | Enterprise, compliance, live ops, competitive benchmarking |
| VII | 42 | 821–840 | Sliced AI engineering workbench and 1.0 launch |

### **4.2 Volume dependency graph**

flowchart LR  
  V01\[01 Vision\] \--\> V03\[03 Toolchain\]  
  V02\[02 Math\] \--\> V05  
  V03 \--\> V04\[04 Fabric\]  
  V04 \--\> V05\[05 Runtime \+ ECS\]  
  V04 \--\> V06\[06 Editor shell\]  
  V05 \--\> V06  
  V05 \--\> V07\[07 Vulkan\]  
  V07 \--\> V08\[08 Render graph\]  
  V08 \--\> V09\[09 Shaders\]  
  V04 \--\> V10\[10 Assets\]  
  V09 \--\> V10  
  V10 \--\> V11\[11 2D\] & V12\[12 Geometry\] & V13\[13 Animation\] & V14\[14 Audio UI VFX\]  
  V05 \--\> V15\[15 Physics \+ net\]  
  V05 \--\> V16\[16 Insights\]  
  V16 \--\> V17\[17 Replay\]  
  V04 \--\> V18\[18 Collab\]  
  V06 \--\> V19\[19 Code \+ graphs\]  
  V19 \--\> V20\[20 Governed AI\]  
  V16 \--\> V21\[21 Verification\]  
  V21 \--\> V22\[22 Build farm\]  
  V18 \--\> V23\[23 Services\]  
  V19 \--\> V24\[24 SDK\]  
  V22 & V24 \--\> V25\[25 Early Access\]  
  V07 \--\> V26\[26 Metal \+ hw lab\]  
  V09 \--\> V27\[27 Geometry \+ GI\] \--\> V28\[28 Film rendering\]  
  V27 \--\> V29\[29 Large worlds\]  
  V12 & V13 \--\> V30\[30 Digital humans\]  
  V15 \--\> V31\[31 Destruction\]   
  V15 & V23 \--\> V32\[32 Gameplay at scale\]  
  V26 \--\> V33\[33 Mobile\] \--\> V34\[34 XR\]  
  V26 \--\> V35\[35 Consoles\]  
  V26 \--\> V36\[36 Web runtime\]  
  V23 \--\> V37\[37 Hub website\]  
  V37 \--\> V38\[38 Marketplace\]  
  V10 \--\> V39\[39 Interchange\]  
  V28 \--\> V40\[40 Virtual production\]  
  V37 \--\> V41\[41 Enterprise \+ bench\]  
  V20 \--\> V42\[42 AI workbench \+ 1.0\]  
  V41 \--\> V42  
The graph is the minimum hard order. Later volumes still shape early decisions: collaboration (18) constrains identity design (04), and replay (17) constrains scheduling and clocks (05). Those constraints are written into SL-ARCH documents before the early volumes freeze.

### **4.3 Implementation phases (vertical slices)**

flowchart LR  
  P0\[P0 Foundation\<br/\>Vol 01-05\] \--\> G0{Gate ep 100\<br/\>runtime}  
  G0 \--\> P1\[P1 Core loop\<br/\>Vol 06-10\] \--\> G1{Gate ep 200\<br/\>asset pipeline}  
  G1 \--\> P2\[P2 Production\<br/\>Vol 11-20\] \--\> G2{Gate ep 400\<br/\>2D \+ 3D games}  
  G2 \--\> P3\[P3 Early Access\<br/\>Vol 21-25\] \--\> G3{Gate ep 500\<br/\>EA release}  
  G3 \--\> P4\[P4 Unreal-class depth\<br/\>Vol 26-32\] \--\> G4{Gate ep 640\<br/\>SL-BENCH parity}  
  G4 \--\> P5\[P5 Platforms \+ web\<br/\>+ ecosystem\<br/\>Vol 33-42\] \--\> G5{Gate ep 840\<br/\>Sliced Labs 1.0}  
Each gate is a running product with recorded evidence, not a document. Gate ep 640 is where the Unreal parity matrix (2.7) is first measured on the hardware lab.

## **5\. Kohi alignment track**

Kohi is the build-along guide for Volumes 03, 05, 07, 08 and 09; Sliced Labs follows its order of work but writes everything in C++23 with its own architecture. [Kohi](https://github.com/travisvroman/kohi) is a C and Vulkan engine by Travis Vroman, built on YouTube and now live on Twitch, split into core, runtime, Vulkan renderer plugin and tools libraries. Watch an arc, design the Sliced Labs version against this plan, then build it.

### **5.1 Arc-to-volume mapping**

Kohi episode numbers below follow a [community index of the series](https://github.com/nullshii/luna) and are approximate.

| Kohi arc | Approx. Kohi episodes | Sliced Labs episodes | Where Sliced Labs deliberately differs |
| :---- | :---- | :---- | :---- |
| Architecture overview and setup | \#000–\#002 | 041–048 | CMake \+ Clang/GCC with C++23 feature tests, lockfiles, CI from day one (Kohi uses its own build scripts in C) |
| Logging and assertions | \#003 | 056 | Structured logs with typed fields and correlation IDs (F6), not just formatted strings |
| Platform layer (Windows, Linux) | \#004–\#005 | 082 | Linux/Wayland first (your Arch workstation); RAII handles; platform behind an interface tested headless |
| Memory subsystem | \#007 | 083–085 | std::pmr\-compatible allocators, tagged allocation diagnostics feeding the profiler |
| Events and input | \#008–\#009 | 097, 108 | Typed event envelopes from the Fabric schema, not a void-pointer event bus |
| Renderer overview, Vulkan bring-up | \#012–\#021 | 121–129 | Capability-negotiated device layer; Vulkan 1.3+ dynamic rendering and synchronization2 instead of classic renderpasses where supported |
| Pipelines, shaders, buffers, first triangle | \#024–\#028 | 130–133, 147, 151 | Shader compile as a tracked build-graph node with provenance (F3) |
| Uniforms and descriptors | \#029 | 130, 174 | Descriptor strategy decided once; bindless path capability-gated |
| Texture, material, geometry, resource systems | \#035–\#038 | 167–170, 181–187 | Every resource gets a canonical ID and a derivation record; no loose resource files |
| Multiple render passes, render targets | \#039, \#059 | 141–145 | A declarative render graph with automatic barriers and aliasing, plus its visualizer (157) |
| Lighting, normal maps | \#049–\#051 | 152–155 | PBR from the start, color-space handling specified |
| Meshes and transforms | \#053–\#054 | 147–149, 221–222 | Editable source mesh kept separate from runtime mesh |
| Camera, views, transparency, skybox | \#060–\#063 | 148, 155 | Views are render-graph inputs, not a separate view system |
| Job system | \#067 | 086–090 | Dependency-aware scheduler with read/write declarations for ECS systems |
| Renderbuffers, dynamic buffers | \#044, \#072 | 126–127, 134 | GPU lifetime manager with deferred retirement |
| Game-code hot reload | Kohi feature | 367–371 | Formal reload protocol with ABI manifest and rollback (section 2.5) |
| Plugins (renderer, audio, UI) | Kohi structure | 375–379 | Versioned plugin ABI with permissions |

Kohi has no ECS-first runtime, canonical project model, transactions, collaboration, replay or governed AI; Volumes 04, 17, 18 and 20 have no Kohi counterpart and are designed from this plan alone.

### **5.2 Weekly build-along loop**

flowchart LR  
  W\[Watch one Kohi arc\] \--\> S\[Write the episode spec\<br/\>purpose, contract, tests\]  
  S \--\> P\[Ready-to-paste\<br/\>Claude Code prompt\]  
  P \--\> B\[Build in C++23\<br/\>in your repo\]  
  B \--\> T{Acceptance tests pass?}  
  T \-- no \--\> B  
  T \-- yes \--\> R\[Record evidence \+\<br/\>tick the episode\]  
  R \--\> W  
Type the core systems yourself at least once, even when Claude Code drafts them. The goal is an engine you understand, not only one that compiles.

## **6\. Episode anatomy and governance**

An episode is accepted only on evidence: passing acceptance tests, an integrated demo in the running product, and a completion record. Code existing is not completion.

### **6.1 Required record for every episode**

| Field | Content |
| :---- | :---- |
| ID / volume | SL-EP-nnn, SL-VOL-nn (stable forever) |
| Purpose | The capability established; user outcome; non-goals |
| Dependencies | Episodes and documents that must be accepted first |
| Decision status | Locked, conditional, experimental or unresolved |
| Architecture | Boundaries, ownership, data flow, failure modes |
| Executable contract | Schemas, API signatures, invariants, error behavior |
| Work orders | Bounded Claude Code prompts with file ownership |
| Acceptance suite | Positive, negative, boundary, recovery, performance cases |
| Integration target | The vertical slice that consumes it |
| Kohi reference | Matching Kohi arc, if any |
| Release applicability | Milestone it ships in (Foundation, Core loop, Production, Early Access, 1.0) and target platforms |
| Completion record | Revision, test evidence, benchmarks, open defects, approved exceptions |

### **6.2 Episode lifecycle**

stateDiagram-v2  
  \[\*\] \--\> Planned  
  Planned \--\> Specified: spec \+ contract written  
  Specified \--\> InProgress: dependencies accepted  
  InProgress \--\> Verifying: implementation done  
  Verifying \--\> InProgress: tests fail  
  Verifying \--\> Demonstrated: tests pass  
  Demonstrated \--\> Accepted: evidence recorded  
  Accepted \--\> Reopened: regression or contract change  
  Reopened \--\> InProgress  
  Verifying \--\> Blocked: dependency regressed  
  Blocked \--\> InProgress

### **6.3 Governance rules**

1. The JSON episode registry (SL-REG-002) is authoritative; this doc and any CSV are views of it.  
2. Episode IDs never change meaning. A changed scope amends the record; a new capability gets a new ID at the end of its volume's reserved range or in an appendix volume.  
3. No silent scope changes: every change carries an impact note, traceability update and an ADR when it alters a locked decision.  
4. Claude Code works through bounded work orders: one episode, declared files, declared tests. Its report of "done" is not evidence; the test run is.  
5. No parity claim ("rivals Unreal" or similar) ships without benchmark evidence from Volume 41\.

## **Part I — Foundations (episodes 001–100)**

Part I ends with a multithreaded ECS simulation, a canonical project model with executable contract tests, and a reproducible toolchain. No rendering yet.

flowchart LR  
  A\[Vol 01\<br/\>Constitution\] \--\> C\[Vol 03\<br/\>Toolchain \+ CI\]  
  B\[Vol 02\<br/\>Math test suite\] \--\> E  
  C \--\> D\[Vol 04\<br/\>Fabric contracts\]  
  D \--\> E\[Vol 05\<br/\>Runtime \+ ECS\]  
  E \--\> G{{Gate ep 100:\<br/\>MT ECS sim with\<br/\>diagnostics}}

### **Volume 01 — Product vision, requirements, competitive architecture**

Goal: define what Sliced Labs is, what it must do, and how completion is measured.

* **001** Define the Sliced Labs product — engine, editor, SDK, runtime, content suite, automation layer and delivery platform as one product family.  
* **002** Establish the competitive position — compare Unreal, Unity, Godot, proprietary engines, DCC tools and standalone dev tools.  
* **003** Define the zero-manual-fragmentation doctrine — native workflows, permitted integrations, F1–F8 measurement.  
* **004** Define users and production scenarios — solo devs, studios, technical artists, simulation engineers, platform developers.  
* **005** Define the capability ontology — canonical inventory of capabilities, dependencies, interfaces, ownership.  
* **006** Define the product boundary — core, optional modules, plugins, external integrations, hosted services.  
* **007** Establish the quality model — reliability, performance, usability, accessibility, security, compatibility.  
* **008** Define the hardware support matrix — minimum, recommended and high-end workstation configurations.  
* **009** Define the target platforms and capability profiles — desktop, mobile, XR, console and browser, all in 1.0 scope.  
* **010** Define the product lifecycle — create, author, execute, debug, collaborate, test, release.  
* **011** Define measurable acceptance criteria — product promises converted to tests and evidence.  
* **012** Establish the engineering decision system — ADRs, status rules, supersession, change control.  
* **013** Establish risk and feasibility registers — technical uncertainty, dependencies, delivery constraints.  
* **014** Define the build-versus-adopt policy — proprietary advantages vs mature infrastructure.  
* **015** Define the dependency policy — licensing, security, support, reproducibility, supply chain.  
* **016** Define the development economics — effort, hardware, distribution, support, hosting, maintenance.  
* **017** Define the extensibility strategy — first-party modules, plugins, native extensions, compatibility.  
* **018** Define the public roadmap — capability milestones and evidence-based release gates.  
* **019** Create the master requirements traceability matrix — requirement → architecture → implementation → test.  
* **020** Approve the Engine Constitution — freeze locked commitments; record conditional decisions.

Deliverable: SL-GOV-001 Engine Constitution, requirements catalogue, capability map, decision register.

### **Volume 02 — Mathematics, computer science, graphics foundations**

Goal: the knowledge needed to design the engine rather than assemble libraries.

* **021** Linear algebra for engines — vectors, matrices, bases, products, numerical stability.  
* **022** Coordinate systems and transforms — local, parent, world, view, clip, device spaces.  
* **023** Quaternions and rotation — composition, interpolation, representations, singularities.  
* **024** Calculus for simulation and graphics — derivatives, integration, gradients, ODEs.  
* **025** Numerical methods — approximation, error propagation, precision, convergence.  
* **026** Discrete math and graph theory — traversal, DAGs, cycles, topological order.  
* **027** Algorithms and data structures — complexity, hashing, trees, heaps, spatial structures, cache behavior.  
* **028** Computer architecture — pipelines, caches, memory hierarchy, SIMD, NUMA.  
* **029** Operating-system fundamentals — processes, virtual memory, threads, file systems, devices.  
* **030** Concurrency theory — memory models, atomics, races, deadlocks, lock-free trade-offs.  
* **031** Compiler fundamentals — lexing, parsing, semantics, IR, optimization, codegen.  
* **032** Type systems — static typing, generics, inference, ownership, API correctness.  
* **033** C++ object and memory model — lifetimes, UB, aliasing, object representation, ABI.  
* **034** Rendering mathematics — projection, clipping, rasterization, sampling, color spaces.  
* **035** Signal processing — sampling, filtering, transforms, convolution, audio basics.  
* **036** Physics mathematics — forces, rigid bodies, collision geometry, constraints, integration.  
* **037** Information theory and compression — entropy, coding, quantization, storage trade-offs.  
* **038** Distributed-systems foundations — consistency, ordering, retries, idempotency, failure models.  
* **039** Determinism and reproducibility — RNG, floating-point differences, stable ordering.  
* **040** Math validation suite — executable tests for transforms, projections, geometry, numerical error, determinism.

Deliverable: sl::math library with a verified numerical test suite.

### **Volume 03 — Toolchain, repository, build infrastructure**

Goal: a reproducible environment before significant engine code. Kohi \#000–\#003.

* **041** Select supported development environments — Arch Linux primary, Windows secondary.  
* **042** Establish the C++ toolchain — compiler, linker, standard library, debugger.  
* **043** Define the C++23 baseline — supported features proven by toolchain tests.  
* **044** Evaluate C++26 capabilities — feature tests and promotion criteria.  
* **045** Configure Vulkan development — SDK, loader, validation layers, diagnostics.  
* **046** Establish the repository architecture — engine, editor, tools, SDK, tests, docs, examples, third\_party.  
* **047** Design the build system — targets, dependency graph, configurations, generated metadata.  
* **048** Lock dependencies reproducibly — lockfiles, manifests, update process.  
* **049** Automated formatting — style, include order, enforcement.  
* **050** Static analysis — warnings-as-errors, sanitizers, analyzers, quality gates.  
* **051** Test infrastructure — unit, integration, system, end-to-end runners.  
* **052** Test fixtures and golden data — versioned assets, expected outputs, deterministic fixtures.  
* **053** Local CI runner — build, test, format and contract checks in one command.  
* **054** Initial CI pipeline — clean builds, caching, artifacts, failure reports.  
* **055** Artifact provenance — source revision, compiler ID, parameters, output hashes (F3 seed).  
* **056** Developer diagnostics — structured logs, error codes, stack traces, machine-readable reports.  
* **057** Documentation generation — API docs, dependency reports, architecture references.  
* **058** Secure dependency ingestion — licenses, checksums, vulnerability findings.  
* **059** Reproducible development image — packaged toolchain and environment.  
* **060** Gate the first infrastructure release — clean checkout builds, tests and emits traceable artifacts.

Deliverable: reproducible toolchain and enforced CI.

### **Volume 04 — Core architecture, identity, project data (the Fabric)**

Goal: the canonical data model that keeps editor, runtime, assets and tools from drifting apart. No Kohi counterpart.

* **061** Canonical identity algebra — stable IDs, typed references, namespaces, identity lifetime.  
* **062** Canonical type system — primitive, compound, tagged, optional, reference, versioned types.  
* **063** Schema versioning — compatibility, migration, deprecation, failure semantics.  
* **064** Project manifest — metadata, capabilities, target profiles, required modules.  
* **065** Asset and entity identities — assets vs entities vs instances vs derived resources.  
* **066** Ownership and authority — which subsystem owns each record and who may mutate it (F2).  
* **067** Provenance and epistemic classes — observed, imported, asserted, calculated, inferred, accepted.  
* **068** Canonical serialization — stable encoding, version tags, validation, corruption handling.  
* **069** Event envelopes — IDs, schema versions, timestamps, causality, correlation.  
* **070** Transaction model — atomic edits, validation, commit boundaries, rollback (F5).  
* **071** Undo/redo semantics — reversible ops, grouping, compensation for irreversible actions.  
* **072** Dependency graph — source dependencies, derived outputs, invalidation, cycles.  
* **073** Change-impact engine — affected assets, systems, tests, build targets.  
* **074** Runtime vs document boundary — persistent project state vs transient simulation state.  
* **075** Reference resolver — IDs across loaded documents, packages, external references.  
* **076** Project recovery — journals, checkpoints, incomplete-transaction detection.  
* **077** Corruption policies — validation failures, quarantine, repair, loss reporting.  
* **078** Schema-generated tooling — serializers, inspectors, validators, API metadata from schemas.  
* **079** Architecture conformance suite — identity, ownership, migration, transaction, dependency invariants.  
* **080** Release the canonical model — approved schemas plus executable contract tests.

Deliverable: SL-ARCH-001 Canonical Project Model and its test suite.

### **Volume 05 — Native runtime, memory, jobs, ECS**

Goal: the runtime that runs games, simulations, editor previews and headless jobs. Kohi \#004–\#009, \#067.

* **081** Runtime lifecycle states — startup, init, run, suspend, shutdown, recovery.  
* **082** Platform abstraction boundary — windowing, timing, input, files, threads.  
* **083** Memory allocation contracts — alignment, lifetime, ownership, instrumentation.  
* **084** Foundational allocators — arena, pool, stack, general-purpose adapters.  
* **085** Allocation diagnostics — allocation sites, leaks, fragmentation, peaks.  
* **086** Job-system prototype — submission, worker queues, dependencies, completion.  
* **087** Synchronization primitives — fences, semaphores, atomic coordination.  
* **088** Work stealing and scheduling — throughput, latency, locality, starvation.  
* **089** Frame and simulation clocks — wall, frame, fixed-step and logical time.  
* **090** Frame scheduler — ordered phases, task dependencies, sync boundaries.  
* **091** ECS entity lifecycle — creation, destruction, generations, stale handles, ID mapping.  
* **092** Archetype component storage — data-oriented layouts.  
* **093** ECS queries — filters, iteration, caching, structural changes.  
* **094** System scheduling contracts — declared reads/writes, ordering, parallelism.  
* **095** Change tracking — modified components, structural changes, invalidation signals.  
* **096** Runtime reflection — type, field, method and serialization metadata for tools.  
* **097** Typed runtime events — validated dispatch, ordering, tracing.  
* **098** Runtime snapshots — selected state for tests, recovery, replay.  
* **099** Concurrency and memory stress tests — scheduler, allocator and ECS invariants.  
* **100** Gate the runtime foundation — multithreaded ECS simulation with diagnostics and reproducible tests.

Deliverable: runtime core, ECS, memory system, job scheduler.

## **Part II — The visible engine (episodes 101–200)**

Part II ends when a changed source asset reliably invalidates, rebuilds and live-reloads only its dependents inside a working editor with a lit 3D scene. This is the P1 core loop.

flowchart LR  
  SRC\[Source asset\] \--\>|import adapter| CAS\[(Content-addressed\<br/\>store)\]  
  CAS \--\> DAG\[Build DAG\]  
  DAG \--\>|shader compile| SH\[SPIR-V \+ reflection\]  
  DAG \--\>|cook| CK\[Runtime artifact\<br/\>+ provenance\]  
  SH \--\> RG\[Render graph\]  
  CK \--\> RG  
  RG \--\> VP\[Editor viewport\]  
  VP \--\>|edit via transaction| SRC

### **Volume 06 — Native editor shell and UI framework**

* **101** Editor application architecture — workspaces, panels, commands, services, lifecycle.  
* **102** UI rendering strategy — immediate vs retained mode against docking, accessibility, theming.  
* **103** UI renderer — draw lists, batching, clipping, GPU resources.  
* **104** Widgets and layout — controls, containers, sizing, constraints.  
* **105** Text rendering — font loading, glyph atlases, shaping, international text, fallback.  
* **106** Docking and panels — splitters, floating windows, multi-monitor, persistence.  
* **107** Command system — one registry for menus, shortcuts, palette, automation and agents.  
* **108** Input routing and focus — keyboard, pointer, navigation, modals.  
* **109** Theme and design tokens — color, spacing, type, density, states (Ash over Snow / Snow over Ash).  
* **110** Accessibility foundations — semantic metadata, keyboard access, scaling, tests.  
* **111** Workspace persistence — layouts, panel state, per-workspace preferences.  
* **112** Shared selection and navigation — assets, entities, code, traces.  
* **113** Property inspector — typed editors generated from schemas.  
* **114** Search and command discovery — indexing, filters, fuzzy match.  
* **115** Notifications and diagnostics UI — errors, warnings, progress, remediation.  
* **116** Editor undo integration — UI commands through the transaction system.  
* **117** Workspace extensions — versioned panel APIs and lifecycles.  
* **118** UI automation hooks — stable widget IDs for automated tests.  
* **119** Project-browser workspace — create, open, inspect, navigate.  
* **120** Gate the editor shell — persistent layouts, accessible navigation, inspector edits, transactional undo.

### **Volume 07 — Vulkan device, resources, synchronization**

Kohi \#012–\#029, \#044, \#072.

* **121** Vulkan feature policy — minimum version and capability negotiation.  
* **122** Instance and diagnostics layer — init with validation and debug utils.  
* **123** Physical devices and features — queues, extensions, memory properties.  
* **124** Logical device — queue selection, only validated features enabled.  
* **125** Surfaces and swapchains — presentation, resize, recreation, minimized windows.  
* **126** GPU memory allocation — heaps, suballocation, alignment, budgets.  
* **127** Buffer and image abstractions — creation, usage declarations, lifecycle.  
* **128** Command recording — pools, buffers, submission, reuse.  
* **129** Explicit synchronization — fences, semaphores, barriers, queue dependencies.  
* **130** Descriptor and binding infrastructure — layouts, access, strategy.  
* **131** Pipeline layouts — descriptor layouts, push constants, compatibility.  
* **132** Graphics pipelines — validated creation, pipeline state caching.  
* **133** Compute pipelines — dispatch and diagnostics.  
* **134** GPU lifetime management — deferred retirement after completion.  
* **135** Device loss and error recovery — failed creation, submission errors, recoverable loss.  
* **136** GPU timestamps — calibrated queue and command timing.  
* **137** Validation dashboard — validation messages tied to resource and command provenance.  
* **138** GPU capture integration — RenderDoc and vendor tools from the editor.  
* **139** Benchmark submission overhead — representative baselines.  
* **140** Gate the graphics foundation — presentation, sync, lifetimes, diagnostics verified.

### **Volume 08 — Render graph and core 3D rendering**

Kohi \#039, \#049–\#063.

* **141** Render-graph semantics — passes, resources, dependencies, boundaries.  
* **142** Render-graph compilation — topological order, cycle rejection.  
* **143** Resource lifetime analysis — transient allocation.  
* **144** Barriers and queue sync — generated from graph dependencies.  
* **145** Transient aliasing — reuse non-overlapping allocations.  
* **146** Frame and scene data model — canonical scene vs renderer-derived state.  
* **147** Vertex and index processing — layouts, buffers, submission.  
* **148** Cameras and projections — perspective, orthographic, configurable models.  
* **149** Transform hierarchy — local/parent/world propagation with invalidation.  
* **150** Frustum culling — view-volume culling, bounds validation.  
* **151** Basic rasterization — depth testing, deterministic pass order.  
* **152** PBR material foundations — parameters, lighting models, color spaces.  
* **153** Direct lighting — directional, point, spot.  
* **154** Shadow mapping — cascades, filtering, diagnostics.  
* **155** Environment lighting — sky, environment maps, IBL.  
* **156** Post-processing — tone mapping, exposure, bloom.  
* **157** Render-graph visualizer — dependencies, transitions, timings, allocations.  
* **158** GPU-driven rendering experiments — indirect draws, GPU culling, capability paths.  
* **159** Rendering and GPU-memory benchmarks — reproducible across hardware.  
* **160** Gate the 3D milestone — lit, animated scene with inspectable passes.

### **Volume 09 — Shaders, materials, textures, advanced rendering**

* **161** Shader language and compiler architecture — Slang/HLSL/GLSL choice, backend portability.  
* **162** Shader compilation service — invocation, diagnostics, structured results.  
* **163** Shader IR — types, bindings, stages, validation.  
* **164** Shader dependency tracking — invalidate only affected shaders and pipelines.  
* **165** Shader artifact caching — keyed by source, compiler, target, features.  
* **166** Shader reflection — layouts, parameters, entry points.  
* **167** Node-based material editor — typed nodes, connections, previews, validation.  
* **168** Material graph compilation — graphs lowered to shader IR.  
* **169** Material parameters — typed values, instances, inheritance, live updates.  
* **170** Texture import and processing — formats, color spaces, mipmaps, compression.  
* **171** Procedural texture graphs — dependency-tracked generator nodes.  
* **172** Shader permutations — variants, specialization, compile budgets.  
* **173** Pipeline-cache management — persistence and failure recovery.  
* **174** Bindless strategies — descriptor indexing with fallbacks.  
* **175** Mesh shaders — capability-gated path vs conventional pipeline.  
* **176** Ray tracing — optional acceleration structures with fallbacks.  
* **177** Volumetric and compute effects — selected volumetrics and compute post.  
* **178** Shader hot reload — interface validation, safe activation.  
* **179** Visual shader debugging — errors, bindings, generated code, pipeline state.  
* **180** Gate materials and shaders — editable materials, invalidation, safe live compile.

### **Volume 10 — Asset database, import, cooking, streaming**

Kohi \#035–\#038.

* **181** Asset ontology — assets, instances, sources, revisions, derivatives, references.  
* **182** Content-addressable storage — immutable objects identified and verified by hash.  
* **183** Asset registry — IDs, types, metadata, revisions.  
* **184** Source import adapters — validated interfaces for glTF, PNG, WAV and more.  
* **185** Importer contracts — provenance, settings, deterministic output, errors.  
* **186** Incremental processing — recompute only affected outputs (F7).  
* **187** Dependency invalidation — revisions propagated through the asset graph (F4).  
* **188** Previews and thumbnails — cached and tied to source revisions.  
* **189** Asset search — types, metadata, tags, dependencies.  
* **190** Asset validation rules — missing refs, invalid formats, stale outputs.  
* **191** Asset cooking — source to target-specific runtime artifacts.  
* **192** Packaging manifests — required resources and completeness.  
* **193** Resource streaming — async load/release under budgets.  
* **194** Residency and eviction — priority, tracking, retirement.  
* **195** Virtualized asset views — huge libraries without unbounded UI memory.  
* **196** Asset migration — schema upgrades preserving identity and provenance.  
* **197** Content integrity checks — hashes, metadata, references.  
* **198** Import/cook budgets — latency, throughput, cache hits, peak memory; F7 baseline set.  
* **199** Asset-pipeline diagnostics workspace — dependencies, stages, cache, invalidation.  
* **200** Gate the asset pipeline — changed source → correct invalidation, rebuild, reload.

## **Part III — Content and games (episodes 201–300)**

Part III ends with a playable networked game authored entirely in Sliced Studio: 2D and 3D content, animation, audio, UI, VFX, physics and gameplay all on the canonical model.

flowchart TB  
  subgraph AUTHOR\[Native authoring workspaces\]  
    S2D\[2D studio\]  
    GEO\[Modeling \+ sculpt\]  
    ANI\[Animation \+ sequencer\]  
    AUD\[Audio\]  
    UI\[Game UI\]  
    VFX\[VFX graph\]  
    WLD\[World tools\]  
  end  
  subgraph SIM\[Simulation\]  
    PHY\[Physics\]  
    GP\[Gameplay components\]  
    AIB\[Game AI\]  
    NET\[Replication\]  
  end  
  AUTHOR \--\>|transactions| FAB\[(Fabric)\]  
  FAB \--\> ECS\[ECS runtime\]  
  ECS \--\> SIM  
  SIM \--\> ECS  
  ECS \--\> PLAY\[Playable networked game\]

### **Volume 11 — Native 2D studio**

* **201** 2D document model — sprites, layers, tiles, frames, palettes, pixel coordinates.  
* **202** 2D renderer — batching, transforms, ordering, clipping, compositing.  
* **203** Sprite editor — pixel grid, selection, drawing primitives, zoom.  
* **204** Color and palettes — indexed color, constraints, transparency.  
* **205** Sprite-sheet authoring — frames, regions, trimming, packing, atlases.  
* **206** Tilemap editing — grids, layers, painting, large-world navigation.  
* **207** Autotiling — adjacency rules, deterministic selection, validation.  
* **208** 2D animation timelines — keyframes, frame durations, tracks.  
* **209** Sprite animation preview — loops, events, speed, runtime state.  
* **210** 2D skeletal rigs — bones, weights, constraints, deformation.  
* **211** 2D inverse kinematics — solvers, constraints, diagnostics.  
* **212** 2D lighting — lights, occluders, normal maps, shadows.  
* **213** Parallax and layered composition — camera-relative layers and depth.  
* **214** SDF graphics — scalable shapes, text, outlines, effects.  
* **215** Pixel-art post-processing — palette mapping, dithering, CRT effects, pixel grid.  
* **216** 2D physics authoring — colliders, layers, joints, overlays.  
* **217** 2D procedural generation — seeded tilemaps and layouts.  
* **218** 2D performance diagnostics — draw calls, batches, overdraw, texture use.  
* **219** Complete 2D game workflow — a playable game with only native tools.  
* **220** Gate the 2D milestone — serialization, undo, runtime, profiling, packaging.

### **Volume 12 — Geometry, modeling, sculpting, mesh authoring**

* **221** Geometry document schemas — topology, surfaces, primitives, derived meshes.  
* **222** Mesh data structures — half-edge connectivity and attributes.  
* **223** Primitive and blockout modeling — parametric primitives and transforms.  
* **224** Polygon editing — selection, extrude, bevel, loop cut, inset, topology checks.  
* **225** Mesh operators — reusable ops with inputs, outputs, failure semantics.  
* **226** Mesh validation — degenerate and non-manifold detection, normals.  
* **227** Subdivision surfaces — Catmull-Clark with creases, performance tests.  
* **228** Sculpting foundations — brush input, deformation, symmetry, undo.  
* **229** Sculpt brush systems — falloff, masks, smoothing, presets, multiresolution.  
* **230** Remeshing — voxel and quad remeshing for production meshes.  
* **231** Retopology — manual and automatic quad retopology, snapping, projection.  
* **232** UV authoring — seams, unwrap, packing, validation.  
* **233** Normals and tangents — smoothing groups, MikkTSpace, baking.  
* **234** Procedural geometry graphs — typed geometry nodes.  
* **235** Simplification and LODs — measured-error LOD chains.  
* **236** Collision-mesh generation — convex decomposition and preview.  
* **237** Geometry interchange — glTF, FBX, USD, OBJ with fidelity reports.  
* **238** Geometry diffing — topology, attributes, transforms.  
* **239** Texture painting and baking — layered painting, normal/AO/curvature bakes.  
* **240** Gate the geometry milestone — model, sculpt, retopo, UV, paint, ship one asset natively.

### **Volume 13 — Animation, cinematics, sequencing**

* **241** Shared animation data model — clips, curves, tracks, rigs, events.  
* **242** Animation curves — interpolation, tangents, evaluation.  
* **243** Animation timeline — tracks, navigation, editing, snapping.  
* **244** Skeletal rigging — skeletons, bind poses, skin weights.  
* **245** Skeletal evaluation — poses, blending, skinning.  
* **246** Animation state machines — states, transitions, conditions.  
* **247** Blend spaces and trees — parameterized blending.  
* **248** Retargeting — skeleton mapping and motion transfer.  
* **249** IK — two-bone, FABRIK, full-body solvers with debugging.  
* **250** Rig graph — Control Rig-class procedural rigging on the graph IR.  
* **251** Root motion — extraction, application, gameplay interaction.  
* **252** Animation events — gameplay, audio, VFX sync.  
* **253** Sliced Sequencer — shots, cameras, tracks, events.  
* **254** Camera animation and blending — rails, cranes, constraints.  
* **255** Recording and takes — capture, compare, manage performances.  
* **256** Timeline synchronization — animation, audio, VFX, gameplay.  
* **257** Live animation debugging — poses, transitions, events.  
* **258** Animation compression and baking — runtime formats.  
* **259** Cinematic regression tests — timelines, camera paths, frames.  
* **260** Gate the animation milestone — gameplay and cinematics on one data model.

### **Volume 14 — Audio, UI, VFX, world authoring**

* **261** Audio asset and event model — sources, clips, buses, events.  
* **262** Audio engine — mixing, voices, real-time scheduling off the game thread.  
* **263** Audio import and waveforms — decode, metadata, display.  
* **264** Waveform editing — trim, fades, regions, nondestructive edits.  
* **265** Mixer and routing graphs — buses, effects, sends.  
* **266** Spatial audio — attenuation, panning, HRTF, occlusion.  
* **267** Interactive music — states, transitions, synced segments.  
* **268** Game UI authoring — layouts, widgets, bindings, responsive design.  
* **269** UI styling and animation — themes, states, transitions.  
* **270** UI preview and input simulation — navigation, scaling, accessibility.  
* **271** VFX graph model — emitters, operators, parameters, stages.  
* **272** CPU particles — spawning, lifetime, deterministic option.  
* **273** GPU particles — compute simulation, collisions, debugging.  
* **274** Procedural VFX authoring — reusable effects and live preview.  
* **275** Splines and volumes — roads, paths, triggers, regions.  
* **276** World composition — placement, grouping, prefabs, assemblies.  
* **277** Procedural placement — seeded scatter rules.  
* **278** Scene streaming and partitioning — loadable regions with dependencies.  
* **279** HLOD — distant representations and transitions.  
* **280** Gate the content milestone — mixed world with audio, animation, UI, VFX.

### **Volume 15 — Physics, gameplay, AI, networking**

* **281** Physics integration contract — fixed step, events, ownership, replay.  
* **282** Jolt adapter — replaceable backend, determinism settings.  
* **283** Collision shape authoring — primitives, meshes, compounds.  
* **284** Rigid bodies — mass, inertia, forces, constraints.  
* **285** Collision queries — raycasts, sweeps, contacts, visualization.  
* **286** Character movement — controllers, slopes, steps, grounded state.  
* **287** Navigation data generation — navmesh as a build-graph node.  
* **288** Pathfinding and agents — queries, avoidance, diagnostics.  
* **289** Gameplay conventions — state, events, system coordination.  
* **290** Gameplay component library — movement, interaction, health, input.  
* **291** Game AI — behavior trees, utility AI, state machines, blackboards.  
* **292** Visual gameplay graphs — typed execution graphs with inspection.  
* **293** Networking model — authority, replication, identity.  
* **294** Transport — reliable-UDP with encryption, QUIC for services.  
* **295** Entity replication — relevancy, serialization, bandwidth control.  
* **296** Prediction and reconciliation — client-side prediction, correction.  
* **297** Server-authoritative simulation — dedicated server builds.  
* **298** Network diagnostics — packets, latency, divergence.  
* **299** Multiplayer chaos tests — loss, delay, disconnect, recovery.  
* **300** Gate the gameplay milestone — playable networked game, inspectable end to end.

## **Part IV — Engine intelligence (episodes 301–400)**

Part IV ends when a recorded session can be rewound to the cause of a bug, two users can merge edits without silent loss, code reloads safely with rollback, and an AI agent completes a bounded task through proposal, tests, review and an accepted transaction.

flowchart LR  
  RUN\[Running world\] \--\>|traces| INS\[Sliced Insights\]  
  RUN \--\>|inputs \+ checkpoints| REP\[Replay store\]  
  REP \--\> RW\[Rewind inspector\]  
  RW \--\>|first divergent tick| CODE\[Code workspace\]  
  CODE \--\>|hot reload protocol| RUN  
  AG\[AI agent\] \--\>|proposal| GOVN{Permissions \+\<br/\>approval}  
  GOVN \--\>|transaction| FAB\[(Fabric)\]  
  FAB \--\> COL\[Collaboration:\<br/\>revisions \+ merges\]  
  FAB \--\> RUN

### **Volume 16 — Debugging, profiling, observability (Sliced Insights)**

* **301** Observability data model — traces, metrics, logs, events, correlation.  
* **302** Structured logging — typed fields, severity, retention, context.  
* **303** CPU sampling — call stacks tied to source.  
* **304** Flame graphs — hot paths and call trees.  
* **305** Frame and system timelines — jobs, systems, waits, phases.  
* **306** Memory profiling — allocations, lifetimes, retention, hotspots.  
* **307** GPU pass profiling — timings per render-graph pass.  
* **308** GPU resource inspection — formats, sizes, transitions.  
* **309** Debugger adapters — breakpoints, stepping, watches via DAP/LLDB/GDB.  
* **310** ECS-aware debugging — entity composition, component values, access.  
* **311** Runtime object navigation — error → entity → asset → source.  
* **312** Capture sessions — bounded traces, logs, snapshots.  
* **313** Network tracing — replication, messages, state.  
* **314** Asset diagnostics — why loaded, invalidated or retained.  
* **315** Performance budgets — target-specific thresholds.  
* **316** Benchmark harnesses — warm-up, repetition, controlled conditions.  
* **317** Regression analysis — confidence-aware baselines.  
* **318** Unified diagnostic workspace — code, runtime, GPU, memory, assets in one view.  
* **319** Profiler overhead and correctness tests.  
* **320** Gate Insights — diagnose a seeded regression in one workspace.

### **Volume 17 — Deterministic simulation and time travel**

* **321** Determinism classes — builds, subsystem replay, whole-world rewind, lockstep.  
* **322** Deterministic random streams — seeds, stream IDs, state capture.  
* **323** Stable ECS iteration — ordering rules for replay-sensitive systems.  
* **324** Fixed-step contracts — tick progression, accumulation, catch-up.  
* **325** Input recording — tick-stamped user input and commands.  
* **326** Event recording and causality.  
* **327** State hashing — divergence detection.  
* **328** Snapshot serialization — schema-aware restore.  
* **329** Incremental checkpoints — deltas and intervals.  
* **330** Replay orchestration — restore and re-execute.  
* **331** Divergence detection — first mismatching tick by entity and component.  
* **332** Rewind inspector — scrub history, inspect values.  
* **333** Branching histories — fork a replay without corrupting the original.  
* **334** Physics replay guarantees — per backend and platform.  
* **335** Deterministic test scenarios — repeatable workloads.  
* **336** External-input capture — files, network, clocks, devices.  
* **337** Retention policies — bounded storage, saved sessions.  
* **338** Replay visualization — state changes and event sequences.  
* **339** Replay under load — long sessions, high entity counts.  
* **340** Gate replay — reproduce, inject divergence, rewind to cause.

### **Volume 18 — Source control, collaboration, history**

* **341** Revision and branch semantics — immutable revisions, merge bases.  
* **342** Revision metadata — author, change sets, evidence.  
* **343** Local history database.  
* **344** Structured diffs — field-level changes.  
* **345** Three-way merging.  
* **346** Graph-aware merges — nodes, edges, parameters.  
* **347** Scene-aware merges — transforms, components, structure.  
* **348** Binary asset versioning in the CAS.  
* **349** Locks and leases for exclusive resources.  
* **350** Concurrent transaction validation — stale edits, conflicts.  
* **351** Live presence — participants, selections, status.  
* **352** Shared editing sessions — real-time operation sync.  
* **353** Collaboration authorization — roles and permissions.  
* **354** Offline editing and reconciliation.  
* **355** Collaboration service — sessions, durable revisions, diagnostics.  
* **356** Provenance across branches.  
* **357** History browsing and recovery.  
* **358** Conflict-resolution UX.  
* **359** Collaboration failure tests — interrupted sessions, server recovery.  
* **360** Gate collaboration — two users edit and merge with no silent loss.

### **Volume 19 — Native code toolchain, graphs, Luau, plugins**

* **361** Code-module boundaries — engine, game, plugin modules.  
* **362** C++ language services — clangd integration, indexing, diagnostics.  
* **363** Native code workspace — editing, navigation, search.  
* **364** Source-to-runtime navigation.  
* **365** Build orchestration — incremental compile, error correlation.  
* **366** ABI and interface manifests.  
* **367** Hot-reload compatibility model — reload classes, prohibited transitions.  
* **368** Isolated candidate compilation.  
* **369** Safe function replacement at boundaries.  
* **370** State migration hooks.  
* **371** Reload rollback and health checks.  
* **372** Graph IR — typed nodes, control flow, dataflow, semantics.  
* **373** Graph compilation — lowering to C++ and Luau bytecode.  
* **374** Graph debugging and source mapping.  
* **375** Luau integration — VM embedding, type checking, ECS bindings.  
* **376** Luau sandboxing and hot reload — permissions, state migration.  
* **377** Plugin ABI and lifecycle — load, unload, versions, isolation.  
* **378** Plugin management and permissions.  
* **379** SDK headers and module templates.  
* **380** Gate the toolchain — edit, debug, reload, graphs, Luau, plugins.

### **Volume 20 — AI-native editor and governed automation**

* **381** AI operating-layer architecture — models, tools, context, policy.  
* **382** AI tool contracts — typed inputs, outputs, permissions, effects.  
* **383** Project-aware context retrieval — schemas, source, assets, diagnostics.  
* **384** Model-provider adapters — hosted and local.  
* **385** Assistant workspace — inside the editor, never a second source of truth.  
* **386** Code-change proposals — reviewable diffs on revisions.  
* **387** Governed asset generation — candidate content with provenance.  
* **388** Agent permissions — per operation, resource and risk.  
* **389** Approval gates for privileged or irreversible actions.  
* **390** Transactional agent edits — same validation and undo as humans.  
* **391** Automated test selection from change impact.  
* **392** Agent diagnostics — bounded read-only investigation.  
* **393** Automated performance investigation.  
* **394** Long-running task orchestration — checkpoints, budgets, cancellation.  
* **395** Isolated agent execution — sandboxes and resource limits.  
* **396** AI provenance and audit history.  
* **397** Multi-agent coordination — file ownership, ordering.  
* **398** Agent evaluation suites — correctness, permission enforcement.  
* **399** Workflow automation — repeatable validated workflows.  
* **400** Gate governed AI — bounded task from proposal to accepted transaction.

## **Part V — Early Access (episodes 401–500)**

Part V ends with Sliced Labs Early Access on Linux and Windows: verified, signed, installable, documented, with an SDK and backend services that the Part VII website builds on.

flowchart LR  
  REV\[Release revision\] \--\> DAG\[Build DAG on\<br/\>local \+ remote workers\]  
  DAG \--\> TEST\[Unit, integration,\<br/\>visual, perf, soak\]  
  TEST \--\> PKG\[Package per target\]  
  PKG \--\> SIGN\[Sign \+ provenance\<br/\>manifest\]  
  SIGN \--\> STAGE\[Staging channel\]  
  STAGE \--\>|qualification report| PROD\[Release channel\]  
  PROD \--\>|crash reports \+ symbols| TEST

### **Volume 21 — Testing, verification, quality engineering**

* **401** Verification architecture — layers, ownership, traceability, gates.  
* **402** Native unit-test framework — fixtures, parameterized tests, structured results.  
* **403** Property-based testing.  
* **404** Fuzzing — parsers, serializers, importers, network input.  
* **405** Sanitizers and memory testing — ASan, UBSan, TSan, leak checks.  
* **406** Integration-test orchestration.  
* **407** Headless engine execution.  
* **408** Editor end-to-end tests — includes F1 workflow acceptance per discipline.  
* **409** Visual regression testing — reference captures with tolerances.  
* **410** Shader and pipeline validation tests.  
* **411** Replay tests — consistency and first-divergence reporting.  
* **412** Physics and simulation regression suites.  
* **413** Networking chaos tests.  
* **414** Asset-pipeline regression tests.  
* **415** Schema and migration tests.  
* **416** Performance regression gates.  
* **417** Soak testing — leaks, exhaustion, deadlocks.  
* **418** Hardware compatibility testing — GPU, driver, CPU matrix.  
* **419** Release qualification reports.  
* **420** Gate verification — independently inspectable qualification report.

### **Volume 22 — Build farm, packaging, deployment, delivery**

* **421** Build orchestration architecture — local, remote, caching, ownership.  
* **422** Build-graph executor — parallel, cancellable, recoverable.  
* **423** Content-addressed build cache.  
* **424** Remote worker registration — capabilities, limits, trust.  
* **425** Distributed scheduling — compile, cook, test.  
* **426** Worker isolation — filesystem, credentials, network.  
* **427** Artifact transfer and verification.  
* **428** Target profiles — features, budgets, options per platform.  
* **429** Platform code generation — compiler, linker, SDK per target.  
* **430** Cooking profiles — textures, meshes, audio, shaders per target.  
* **431** Package assembly with dependency manifests.  
* **432** Runtime configurations — development, shipping, dedicated server.  
* **433** Artifact signing with protected credentials.  
* **434** Release manifests and provenance (F3).  
* **435** Release promotion — dev → staging → production.  
* **436** Crash reporting and symbol management.  
* **437** Updates and patching — delta patches.  
* **438** Store submission adapters — Steam, Epic Games Store, itch.io, Microsoft Store.  
* **439** Rollback and incident procedures.  
* **440** Gate delivery — build, test, package, sign, promote through the native workflow.

### **Volume 23 — Backend services, identity, operations**

* **441** Service architecture — local engine vs Hub services.  
* **442** API contracts — versioning, errors, pagination (OpenAPI \+ gRPC).  
* **443** Identity and authentication — OIDC, sessions, service identities, passkeys.  
* **444** Authorization and roles — org, project, resource, admin.  
* **445** Control-plane data model — orgs, projects, subscriptions, workers.  
* **446** Durable persistence — PostgreSQL schema, migrations, backups.  
* **447** Collaboration API.  
* **448** Artifact and package APIs on object storage.  
* **449** Account and organization management.  
* **450** Project provisioning, migration, archive.  
* **451** Remote build-job APIs.  
* **452** Usage and quota enforcement.  
* **453** Operational telemetry — OpenTelemetry metrics, traces, logs.  
* **454** Secrets management.  
* **455** Service deployment automation — Kubernetes, GitOps, rollbacks.  
* **456** Backups and restore testing.  
* **457** Disaster recovery — recovery objectives, drills.  
* **458** Service-level objectives and alerting.  
* **459** Admin operations tooling.  
* **460** Gate services — org creates project, collaborates, triggers remote build.

### **Volume 24 — SDK, plugins, security, compatibility**

* **461** Public SDK architecture and stability commitments.  
* **462** SDK packaging.  
* **463** API compatibility checks and deprecation policy.  
* **464** Plugin templates and samples.  
* **465** Plugin development workflow.  
* **466** Plugin registry.  
* **467** Plugin signing and trust.  
* **468** Extension compatibility testing.  
* **469** Sandbox and capability boundaries.  
* **470** Threat modeling — files, importers, plugins, agents, services, workers.  
* **471** Secure asset ingestion — malicious files, traversal, exhaustion.  
* **472** Vulnerability management.  
* **473** Supply-chain attestations — SLSA-level provenance.  
* **474** Privacy and telemetry controls — consent, minimization, opt-out.  
* **475** Crash and security-report handling.  
* **476** Localization infrastructure — UI, assets, formatting.  
* **477** Accessibility conformance testing.  
* **478** Project compatibility and migration tooling.  
* **479** Security response and patch policy.  
* **480** Gate SDK and security — third-party plugin built, validated, distributed, loaded safely.

### **Volume 25 — Documentation, reference games, Early Access launch**

* **481** Product strategy — customers, tiers, pricing, license (royalty and seat models decided here).  
* **482** Real-workflow validation with outside studios.  
* **483** Release channels — nightly, preview, stable, LTS.  
* **484** Versioning and migration policy.  
* **485** Installer and updater — Linux and Windows.  
* **486** Templates and starter content — 2D, 3D, multiplayer, simulation.  
* **487** User documentation.  
* **488** SDK and API documentation.  
* **489** Onboarding — guided tutorials, contextual help.  
* **490** Sample-project verification suite.  
* **491** Independent architecture and code review.  
* **492** Production-readiness testing.  
* **493** Support operations — triage, diagnostics, escalation.  
* **494** Opt-in product analytics.  
* **495** Distribution infrastructure — installers, checksums, signatures, notes.  
* **496** Reference game 1: 2D platformer built in Sliced Labs.  
* **497** Reference game 2: 3D multiplayer arena built in Sliced Labs.  
* **498** Closed preview and public beta.  
* **499** Early Access release-candidate gate.  
* **500** Ship Sliced Labs Early Access (Linux \+ Windows).

## **Part VI — Unreal-class depth (episodes 501–640)**

Part VI ends at the SL-BENCH parity gate: on the hardware lab, Sliced Labs matches or beats Unreal Engine 5 on the reference scenes for frame time, memory and edit-to-pixel latency, for every row of the parity matrix in section 2.7.

flowchart TB  
  MESH\[Editable source mesh\] \--\>|build graph| CL\[Cluster hierarchy \+\<br/\>LOD DAG\]  
  CL \--\> VIS\[GPU visibility \+\<br/\>software raster\]  
  VIS \--\> VB\[Visibility buffer\]  
  VB \--\> MAT\[Layered material eval\]  
  SC\[Surface cache\] \--\> GI\[Dynamic GI \+\<br/\>reflections\]  
  GI \--\> MAT  
  VSM\[Virtual shadow pages\] \--\> MAT  
  MAT \--\> UP\[Temporal upscaler\]  
  UP \--\> OUT\[Frame\]  
  PT\[Path tracer\] \--\>|reference| OUT

### **Volume 26 — macOS Metal backend, portability, hardware lab**

* **501** Backend contract audit — what Vulkan assumptions leaked; fix them.  
* **502** Metal device and command layer.  
* **503** Metal resources, heaps, argument buffers.  
* **504** Metal synchronization and render-graph mapping.  
* **505** Slang → Metal shader path and reflection.  
* **506** macOS platform layer — windowing, input, Apple silicon specifics.  
* **507** macOS editor packaging, notarization, signing.  
* **508** Cross-backend conformance suite — pixel diffs Vulkan vs Metal.  
* **509** Windows platform hardening — DPI, IME, high-refresh, HDR.  
* **510** Linux distribution matrix — Wayland, X11, Flatpak, AppImage.  
* **511** HDR output on all desktop platforms.  
* **512** Hardware lab design — GPUs from all three vendors, CPU tiers, memory tiers.  
* **513** Automated lab runner — scheduled benchmarks on every machine.  
* **514** SL-BENCH reference scenes — defined and versioned.  
* **515** Unreal comparison methodology — same content, same targets, published method.  
* **516** Driver and OS compatibility tracking.  
* **517** Power and thermal measurement for laptops and handhelds.  
* **518** Steam Deck and handheld PC profile.  
* **519** Portability performance budgets per backend.  
* **520** Gate portability — editor and runtime pass conformance on Linux, Windows, macOS.

### **Volume 27 — Cluster geometry, dynamic GI, virtual shadows**

* **521** Visibility-buffer renderer architecture.  
* **522** Mesh clustering and cluster hierarchy build.  
* **523** LOD DAG with bounded screen-space error.  
* **524** Cluster streaming and residency.  
* **525** GPU two-pass occlusion culling.  
* **526** Software rasterizer for micro-triangles.  
* **527** Hardware path via mesh shaders where supported.  
* **528** Material evaluation from the visibility buffer.  
* **529** Cluster geometry for foliage and masked materials.  
* **530** Surface cache for indirect lighting.  
* **531** Software ray tracing against distance fields.  
* **532** Hardware ray-traced GI and reflections.  
* **533** Screen-space tracing and denoisers.  
* **534** GI quality tiers — high-end, console-class, mobile.  
* **535** Virtual shadow map page allocation.  
* **536** Shadow page caching and invalidation.  
* **537** Contact shadows and soft shadow filtering.  
* **538** Editing while streaming — source mesh edits rebuild only affected clusters.  
* **539** Debug views — clusters, LOD error, GI probes, shadow pages.  
* **540** Gate — film-scale scene at target frame time, measured vs Unreal.

### **Volume 28 — Film-quality rendering**

* **541** Temporal anti-aliasing and upscaler.  
* **542** Vendor upscaler and frame-generation adapters (DLSS, FSR, XeSS).  
* **543** Layered material model — slabs, coatings, subsurface, sheen.  
* **544** Material tier lowering for mobile and web.  
* **545** Subsurface scattering for skin.  
* **546** Physically based sky and atmosphere.  
* **547** Volumetric clouds and fog.  
* **548** Hair and strand rendering.  
* **549** Eye, cornea and refraction shading.  
* **550** Translucency and order-independent transparency.  
* **551** Physically based camera — exposure, depth of field, motion blur, lens effects.  
* **552** Path tracer — unidirectional with MIS, all materials.  
* **553** Path tracer denoising and progressive refinement.  
* **554** Reference mode — real-time vs path-traced comparison in editor.  
* **555** Color pipeline — ACES / OpenColorIO, wide gamut, HDR.  
* **556** Decals and deferred decals.  
* **557** Stylized rendering — toon, painterly, line art as first-class modes.  
* **558** Render queue for final frames with provenance.  
* **559** Rendering performance tiers and scalability settings.  
* **560** Gate — path-traced and real-time frames match within tolerance.

### **Volume 29 — Terrain, water, large worlds, procedural generation**

* **561** Terrain data model — heightfields, layers, holes, transactional edits.  
* **562** Terrain sculpting and painting tools.  
* **563** Terrain rendering — virtual texturing, tessellation, LOD.  
* **564** Runtime virtual texturing.  
* **565** Foliage and instancing at scale.  
* **566** Water bodies — oceans, rivers, lakes, shorelines.  
* **567** Water simulation and buoyancy.  
* **568** World streaming grid on canonical entity IDs.  
* **569** Large-world coordinates — 64-bit positions, origin rebasing.  
* **570** Data layers and world variants.  
* **571** Per-entity storage for merge-friendly worlds.  
* **572** Procedural graph framework — seeded, deterministic, diffable.  
* **573** Procedural biomes and scattering.  
* **574** Procedural buildings and roads.  
* **575** Geospatial import — elevation data, georeferencing.  
* **576** World-scale navigation streaming.  
* **577** HLOD generation at world scale.  
* **578** World-partition collaboration — concurrent region editing.  
* **579** Open-world performance budgets.  
* **580** Gate — 16 km × 16 km streamed world edited by two users live.

### **Volume 30 — Characters and digital humans**

* **581** Character asset model — body, face, hair, clothing, rig.  
* **582** Scan and photogrammetry import.  
* **583** Facial rig — blendshapes and joint-based hybrid.  
* **584** Face animation — capture, retargeting, lip sync.  
* **585** Body rig graph — Control Rig-class procedural rigging.  
* **586** Full-body IK and retargeter.  
* **587** Motion matching database and runtime.  
* **588** Physics-based animation and ragdoll blending.  
* **589** Strand hair authoring — grooming tools in-engine.  
* **590** Hair simulation.  
* **591** Clothing authoring and simulation.  
* **592** Skin shading presets and texture authoring.  
* **593** Character customization graphs — variants as derived artifacts.  
* **594** Digital-human creator workspace — parametric heads and bodies.  
* **595** Character LOD — down to crowd scale.  
* **596** Crowd animation with shared ECS.  
* **597** Conversational characters — AI dialogue with governed tools.  
* **598** Character performance budgets per platform.  
* **599** Character regression tests.  
* **600** Gate — a digital human authored, animated and shipped entirely in Sliced Labs.

### **Volume 31 — Destruction, cloth, fluids, vehicles**

* **601** Destruction asset model — fracture as derived data.  
* **602** Voronoi and procedural fracture tools.  
* **603** Destruction runtime — clusters, strain, connectivity.  
* **604** Destruction networking and replay.  
* **605** Debris, dust and VFX coupling.  
* **606** Rigid-body solver extensions for destruction.  
* **607** Cloth solver — XPBD.  
* **608** Cloth authoring — painting constraints.  
* **609** Soft bodies.  
* **610** GPU fluid simulation — SPH / FLIP.  
* **611** Smoke and fire simulation.  
* **612** Fluid caching and playback.  
* **613** Vehicle physics — wheels, suspension, drivetrain.  
* **614** Vehicle authoring tools.  
* **615** Ropes, chains, cables.  
* **616** Physics fields — forces, strain, anchors.  
* **617** Physics determinism contracts for each solver.  
* **618** Physics profiling and visualization.  
* **619** Physics stress and regression tests.  
* **620** Gate — destructible, driveable, simulated scene at target frame time.

### **Volume 32 — Gameplay at scale**

* **621** Ability system — abilities, effects, attributes, tags.  
* **622** Ability replication and prediction.  
* **623** Crowds on the ECS — thousands of agents.  
* **624** Crowd navigation and avoidance.  
* **625** Smart objects and interaction.  
* **626** Sound graphs — procedural audio on the graph IR.  
* **627** Sample-accurate audio replay.  
* **628** Scaled replication — prioritization, delta compression, interest management.  
* **629** Dedicated-server fleet orchestration.  
* **630** Online services: identity linking and presence.  
* **631** Lobbies and matchmaking.  
* **632** Leaderboards, achievements, stats.  
* **633** Cloud saves and player data.  
* **634** Voice and text chat.  
* **635** Anti-cheat architecture — server authority, integrity checks.  
* **636** Game-mode and save-game frameworks.  
* **637** Gameplay framework samples.  
* **638** Live-game telemetry hooks.  
* **639** Scale tests — 100-player sessions.  
* **640** Gate SL-BENCH parity — every parity-matrix row measured; Unreal-class depth accepted.

## **Part VII — Platforms, web and ecosystem (episodes 641–840)**

Part VII ends with Sliced Labs 1.0: one project shipping to desktop, mobile, XR, console and browser, a live Sliced Hub website with marketplace and cloud builds, virtual production support, enterprise controls, and published benchmarks.

flowchart LR  
  PROJ\[(One Sliced project)\] \--\> BUILD\[Cloud build farm\]  
  BUILD \--\> D\[Desktop\<br/\>Win, Linux, macOS\]  
  BUILD \--\> M\[Mobile\<br/\>Android, iOS\]  
  BUILD \--\> X\[XR\<br/\>Quest, Android XR, Vision Pro\]  
  BUILD \--\> C\[Consoles\<br/\>PS, Xbox, Switch 2\]  
  BUILD \--\> W\[Browser\<br/\>WASM \+ WebGPU\]  
  PROJ \<--\> HUB\[Sliced Hub website\]  
  HUB \--\> MK\[Marketplace\]  
  HUB \--\> WS\[Web Studio \+ Stream\]  
  HUB \--\> ON\[Online services\]

### **Volume 33 — Mobile: Android and iOS**

* **641** Mobile capability profile — GPU tiers, memory, thermal.  
* **642** Android platform layer — activity lifecycle, input, storage.  
* **643** Android Vulkan backend tuning — tile-based GPUs.  
* **644** iOS platform layer — UIKit, lifecycle, input.  
* **645** iOS Metal backend tuning.  
* **646** Mobile rendering path — forward+, tier-lowered materials.  
* **647** Mobile texture compression — ASTC, ETC2.  
* **648** Touch input, gestures, virtual controls.  
* **649** Mobile audio sessions and interruptions.  
* **650** Battery and thermal governors.  
* **651** App size budgets and on-demand content.  
* **652** In-app purchases and store APIs.  
* **653** Mobile push notifications and deep links.  
* **654** Android packaging — AAB, signing, Play submission.  
* **655** iOS packaging — Xcode project gen, signing, App Store submission.  
* **656** On-device profiling.  
* **657** Remote preview on device from the editor.  
* **658** Mobile device-farm testing.  
* **659** Mobile reference game.  
* **660** Gate mobile — reference game shipped to both stores' test tracks.

### **Volume 34 — XR**

* **661** OpenXR integration architecture.  
* **662** Stereo rendering — multiview, single-pass.  
* **663** Foveated rendering — fixed and eye-tracked.  
* **664** Reprojection and frame pacing.  
* **665** Hand tracking and controllers.  
* **666** Passthrough and mixed reality.  
* **667** Spatial anchors and scene understanding.  
* **668** Meta Quest target.  
* **669** Android XR target.  
* **670** Apple Vision Pro target — visionOS, RealityKit interop.  
* **671** PC VR via OpenXR.  
* **672** XR interaction toolkit — grab, teleport, UI rays.  
* **673** XR comfort settings and accessibility.  
* **674** Spatial audio for XR.  
* **675** In-headset editing — place and tweak scenes in VR.  
* **676** XR performance budgets — 72/90/120 Hz.  
* **677** XR profiling.  
* **678** XR testing and automation.  
* **679** XR reference experience.  
* **680** Gate XR — reference experience on Quest, Android XR and Vision Pro.

### **Volume 35 — Consoles**

* **681** Console program enrollment — PlayStation, Xbox, Nintendo developer agreements.  
* **682** NDA code isolation — private platform modules outside the public repo.  
* **683** Xbox — GDK, DirectX 12 backend.  
* **684** PlayStation — platform graphics API backend.  
* **685** Nintendo Switch 2 — NVN backend.  
* **686** Console memory and storage models.  
* **687** Console input, users and accounts.  
* **688** Console save data and trophies/achievements.  
* **689** Console networking and sessions.  
* **690** Console packaging and submission tooling.  
* **691** Certification requirement tracking (TRC/XR/Lotcheck checklists).  
* **692** Automated certification pre-checks.  
* **693** Console performance budgets.  
* **694** Console profiling with platform tools.  
* **695** Devkit farm integration with the build farm.  
* **696** Cross-play and cross-progression.  
* **697** Console localization and ratings metadata.  
* **698** Console reference game port.  
* **699** Console patching flows.  
* **700** Gate consoles — reference game passes pre-certification on all three.

### **Volume 36 — Web runtime, Web Studio, Sliced Stream**

* **701** WebAssembly build of the runtime — Emscripten, threads, SIMD.  
* **702** WebGPU backend.  
* **703** Browser platform layer — canvas, input, audio worklets, storage.  
* **704** Web asset streaming — HTTP range, CDN, caching.  
* **705** Web build size budgets.  
* **706** Publish-to-web from the editor.  
* **707** Sliced Web Studio shell — React app hosting the WASM engine.  
* **708** Web Studio viewing and review — scenes, assets, history.  
* **709** Web Studio commenting on entities and assets.  
* **710** Web Studio light editing through the same transaction API.  
* **711** Web Studio real-time collaboration with desktop users.  
* **712** Sliced Stream — WebRTC cloud rendering of editor and games.  
* **713** Stream GPU fleet and session scheduling.  
* **714** Stream input, latency and quality adaptation.  
* **715** Embeddable web player for portfolios and stores.  
* **716** Web security — origin isolation, CSP, sandboxing.  
* **717** Web accessibility.  
* **718** Cross-browser test matrix.  
* **719** Web reference game.  
* **720** Gate web — project opened, reviewed, edited in a browser and streamed.

### **Volume 37 — Sliced Hub full-stack website**

* **721** Information architecture — public site, account area, org area, admin.  
* **722** Design system for web — tokens shared with the editor.  
* **723** Next.js app foundation — routing, server components, i18n.  
* **724** Marketing site and product pages.  
* **725** Sign-up, sign-in, passkeys, SSO.  
* **726** Organization and team management.  
* **727** Project dashboard — revisions, builds, activity.  
* **728** Billing and subscriptions.  
* **729** Downloads and launcher — editor installs and updates.  
* **730** Cloud build UI — trigger, watch, download artifacts.  
* **731** Documentation portal — versioned, searchable, API reference.  
* **732** Learn — courses and the Sliced build-along series.  
* **733** Community forums and showcases.  
* **734** Public issue tracker and roadmap.  
* **735** Notifications and email.  
* **736** Status page and incident communication.  
* **737** Web performance budgets — Core Web Vitals.  
* **738** Web end-to-end tests — Playwright.  
* **739** Web security review and penetration test.  
* **740** Gate Hub — public launch of the website with accounts, projects, builds, docs.

### **Volume 38 — Marketplace, asset library, creator economy**

* **741** Marketplace data model — listings, versions, licenses.  
* **742** Seller onboarding and verification.  
* **743** Package format with provenance and compatibility proofs.  
* **744** Automated package validation.  
* **745** Storefront — search, filters, previews.  
* **746** In-editor marketplace browser.  
* **747** Payments, tax and payouts.  
* **748** Licensing and entitlements.  
* **749** Reviews, ratings, moderation.  
* **750** First-party asset library — scanned materials, props, environments.  
* **751** Asset library capture pipeline.  
* **752** Plugin distribution through the marketplace.  
* **753** Creator runtime — sandboxed Luau experiences.  
* **754** Creator publishing and discovery.  
* **755** Creator revenue sharing.  
* **756** Content safety and IP protection.  
* **757** Marketplace analytics for sellers.  
* **758** Refunds and disputes.  
* **759** Marketplace scale tests.  
* **760** Gate marketplace — buy, install, update, refund end to end.

### **Volume 39 — Interchange, migration, Live Link**

* **761** OpenUSD import and export with layers.  
* **762** glTF 2.0 full fidelity.  
* **763** FBX import.  
* **764** Alembic caches.  
* **765** MaterialX import.  
* **766** OpenEXR and OpenImageIO pipelines.  
* **767** CAD and BIM import — STEP, IFC, tessellation.  
* **768** Point clouds and Gaussian splats.  
* **769** Unreal project migration — assets, materials, levels.  
* **770** Unity project migration — scenes, prefabs, materials.  
* **771** Godot project migration.  
* **772** Blueprint and C\# script translation assistants (AI-assisted, reviewed).  
* **773** DCC live sync — Blender, Maya, Houdini bridges.  
* **774** Live Link protocol — streaming transforms and curves.  
* **775** Motion capture integration.  
* **776** Face capture from phones and cameras.  
* **777** Fidelity reports for every import.  
* **778** Round-trip tests.  
* **779** Migration guides.  
* **780** Gate interchange — a real Unreal sample project migrated and running.

### **Volume 40 — Virtual production, broadcast, film output**

* **781** Virtual production architecture.  
* **782** Multi-display cluster rendering.  
* **783** LED wall calibration and color.  
* **784** Camera tracking integration.  
* **785** Genlock and frame synchronization.  
* **786** In-camera VFX frustum rendering.  
* **787** Stage operator UI.  
* **788** Virtual scouting in XR.  
* **789** Lens and camera metadata.  
* **790** Broadcast graphics and real-time compositing.  
* **791** SDI/NDI video I/O.  
* **792** Timecode and take management.  
* **793** Final-frame render queue with EXR, ProRes, ACES.  
* **794** Render farm distribution for final frames.  
* **795** Editorial interchange — OTIO, EDL.  
* **796** Animation and previs workflows.  
* **797** Stage recovery and failover.  
* **798** Virtual production tests.  
* **799** Reference short film.  
* **800** Gate virtual production — LED stage session and final-frame delivery.

### **Volume 41 — Enterprise, compliance, live ops, competitive benchmarking**

* **801** Enterprise deployment — on-prem Hub, air-gapped installs.  
* **802** SSO, SCIM, role-based access.  
* **803** Audit logs and compliance exports.  
* **804** SOC 2 control program.  
* **805** GDPR and data residency.  
* **806** Licensing server and seat management.  
* **807** Long-term-support branches.  
* **808** Enterprise support SLAs.  
* **809** Live-ops content delivery — patches, events, A/B tests.  
* **810** Game backend dashboards.  
* **811** Incident response for live games.  
* **812** SL-BENCH public suite — scenes, scripts, results.  
* **813** Published Unreal and Unity comparisons with methodology.  
* **814** Independent benchmark audit.  
* **815** Iteration-speed benchmarks — edit-to-pixel, reload, rebuild.  
* **816** Fragmentation scorecard publication (F1–F8).  
* **817** Customer production case studies.  
* **818** Accessibility certification.  
* **819** Legal — EULA, privacy policy, export compliance.  
* **820** Gate enterprise — audited, compliant, benchmarked.

### **Volume 42 — Sliced AI engineering workbench and 1.0 launch**

* **821** Workbench CLI agent loop.  
* **822** Workbench tool contracts mapped to the editor command system.  
* **823** Context management for large projects.  
* **824** Sandboxed execution environments.  
* **825** Permission profiles and approval UX.  
* **826** Subagents with file ownership.  
* **827** Multi-agent coordination on one project.  
* **828** Skills — packaged, versioned agent procedures.  
* **829** Patch review with evidence attached.  
* **830** Failure recovery and resumable tasks.  
* **831** Agent benchmarks on real engine tasks.  
* **832** 1.0 feature freeze and stabilization.  
* **833** 1.0 platform qualification on every target.  
* **834** 1.0 documentation complete.  
* **835** 1.0 migration path from Early Access.  
* **836** Launch marketing and website release.  
* **837** Launch-day operations.  
* **838** 1.0 release-candidate gate.  
* **839** Ship Sliced Labs 1.0.  
* **840** Continuous evolution program — architecture governance after 1.0.

## **Required documents and registries**

The program needs 104 controlled documents: 62 named below plus one specification per volume (SL-VOL-01-SPEC to SL-VOL-42-SPEC). Each has a stable ID, an owner, dependencies and an approval record; none is complete until its contracts have tests.

### **Governance (SL-GOV)**

| ID | Document | Written in |
| :---- | :---- | :---- |
| SL-GOV-001 | Platform Constitution and Engineering Doctrine | Vol 01 |
| SL-GOV-002 | Requirements Catalogue | Vol 01 |
| SL-GOV-003 | Capability Ontology and Map | Vol 01 |
| SL-GOV-004 | Architecture Decision Record log | Vol 01, ongoing |
| SL-GOV-005 | Risk and Feasibility Register | Vol 01 |
| SL-GOV-006 | Fragmentation Budget Specification (F1–F8) | Vol 01 |
| SL-GOV-007 | Release Gates and Capability Profiles | Vol 01 |
| SL-GOV-008 | Build-vs-Adopt and Dependency Policy | Vol 01 |
| SL-GOV-009 | Quality Model | Vol 01 |
| SL-GOV-010 | Unreal Parity Matrix and Benchmark Policy | Vol 01, Vol 26 |

### **Architecture (SL-ARCH)**

| ID | Document | Written in |
| :---- | :---- | :---- |
| SL-ARCH-001 | Canonical Project Model (Fabric) | Vol 04 |
| SL-ARCH-002 | Transaction, Undo and Recovery Protocol | Vol 04 |
| SL-ARCH-003 | Runtime and Execution Architecture | Vol 05 |
| SL-ARCH-004 | Editor Shell and UI Framework | Vol 06 |
| SL-ARCH-005 | Graphics Backend Contract (Vulkan, Metal, WebGPU, consoles) | Vol 07, 26 |
| SL-ARCH-006 | Render Graph and Renderer | Vol 08 |
| SL-ARCH-007 | Shader and Material System (Slang) | Vol 09 |
| SL-ARCH-008 | Asset Database and Build Graph | Vol 10 |
| SL-ARCH-009 | Content Production Domains (2D, geometry, animation, audio, UI, VFX) | Vol 11–14 |
| SL-ARCH-010 | Simulation, Physics and Networking | Vol 15 |
| SL-ARCH-011 | Observability and Insights | Vol 16 |
| SL-ARCH-012 | Determinism and Replay Contracts | Vol 17 |
| SL-ARCH-013 | Collaboration and Revision Store | Vol 18 |
| SL-ARCH-014 | Code Toolchain, Graph IR, Luau, Hot Reload | Vol 19 |
| SL-ARCH-015 | AI Governance and Agent Tooling | Vol 20, 42 |
| SL-ARCH-016 | Services and Identity | Vol 23 |
| SL-ARCH-017 | SDK, Plugin ABI and Security Model | Vol 24 |
| SL-ARCH-018 | Cluster Geometry, GI and Shadows | Vol 27 |
| SL-ARCH-019 | Film-Quality Rendering and Color Pipeline | Vol 28 |
| SL-ARCH-020 | Large Worlds and Procedural Generation | Vol 29 |
| SL-ARCH-021 | Characters and Digital Humans | Vol 30 |
| SL-ARCH-022 | Advanced Physics | Vol 31 |
| SL-ARCH-023 | Gameplay Frameworks and Online Services | Vol 32 |
| SL-ARCH-024 | Mobile, XR and Console Platform Layers | Vol 33–35 |
| SL-ARCH-025 | Web Runtime, Web Studio and Stream | Vol 36 |
| SL-ARCH-026 | Sliced Hub Web Platform | Vol 37 |
| SL-ARCH-027 | Marketplace and Creator Runtime | Vol 38 |
| SL-ARCH-028 | Interchange and Migration | Vol 39 |
| SL-ARCH-029 | Virtual Production | Vol 40 |
| SL-ARCH-030 | Enterprise Deployment | Vol 41 |

### **Planning, operations and registries**

| ID | Document | Written in |
| :---- | :---- | :---- |
| SL-PLAN-001 | Master Plan (this document) | Now |
| SL-PLAN-002 | Episode Dependency Graph | Vol 01 |
| SL-PLAN-003 | Claude Code Work-Order Protocol and prompt templates | Vol 03 |
| SL-PLAN-004 | Vertical Slice Definitions | Vol 01 |
| SL-PLAN-005 | Kohi Alignment Map | Now, maintained |
| SL-OPS-001 | CI and Quality Gates | Vol 03 |
| SL-OPS-002 | Dependency and Supply-Chain Operations | Vol 03, 24 |
| SL-OPS-003 | Security Response | Vol 24 |
| SL-OPS-004 | Privacy and Telemetry | Vol 24 |
| SL-OPS-005 | Backup and Disaster Recovery | Vol 23 |
| SL-OPS-006 | Release Management | Vol 22 |
| SL-OPS-007 | Performance and Benchmark Operations (hardware lab) | Vol 26 |
| SL-OPS-008 | Agent Permission Policy | Vol 20 |
| SL-OPS-009 | Support Operations | Vol 25 |
| SL-OPS-010 | Live Operations Runbooks | Vol 41 |
| SL-REG-001 | Document Register (JSON) | Now |
| SL-REG-002 | Episode Registry (JSON, authoritative) | Now |
| SL-REG-003 | Requirements Traceability Matrix | Vol 01 |
| SL-REG-004 | Capability Register | Vol 01 |
| SL-REG-005 | Dependency and License Register | Vol 03 |
| SL-REG-006 | Exception Register (F-budget exemptions) | Vol 01 |
| SL-REG-007 | Benchmark Results Register | Vol 26 |

### **Authoring order**

flowchart LR  
  G1\[SL-GOV-001\<br/\>Constitution\] \--\> G6\[SL-GOV-006\<br/\>Fragmentation\]  
  G1 \--\> R2\[SL-REG-002\<br/\>Episode registry\]  
  G1 \--\> A1\[SL-ARCH-001\<br/\>Fabric\]  
  A1 \--\> A2\[SL-ARCH-002\<br/\>Transactions\]  
  A2 \--\> A3\[SL-ARCH-003\<br/\>Runtime\]  
  A3 \--\> A5\[SL-ARCH-005\<br/\>Graphics contract\]  
  A2 \--\> A8\[SL-ARCH-008\<br/\>Build graph\]  
  R2 \--\> P3\[SL-PLAN-003\<br/\>Work orders\]  
  A5 & A8 & P3 \--\> CODE\[Start Volume 03 code\]

## **Release gates, first vertical slice, next steps**

Sliced Labs 1.0 ships only when all seven gates pass with recorded evidence on every platform in section 2.6.

| Gate | Passes when | Evidence |
| :---- | :---- | :---- |
| 1 Architectural integrity | Every authoritative record has owner, stable ID, revisions; F1–F8 at threshold | Conformance suite, fragmentation scorecard |
| 2 Production capability | Reference games (2D, 3D multiplayer, mobile, XR, console, web) and a digital human are made entirely in Sliced Labs | Reference projects \+ F8 workflow tests |
| 3 Engineering reliability | Reproducible builds, migrations, crash recovery, soak and security tests pass | Qualification report |
| 4 Platform readiness | Every target passes conformance, store and certification pre-checks | Platform reports |
| 5 Web and services | Hub, Web Studio, Stream, marketplace and online services meet SLOs | SLO dashboards, pen test |
| 6 Operational readiness | Install, update, support, incident and rollback procedures drilled | Drill records |
| 7 Competitive validation | Every parity-matrix row matches or beats Unreal Engine 5 on SL-BENCH | Audited benchmark register |

### **First vertical slice (target: end of Volume 10\)**

flowchart LR  
  A\[Create project\] \--\> B\[Author scene\] \--\> C\[Import asset\]  
  C \--\> D\[Build DAG\] \--\> E\[Run in editor\<br/\>+ headless\]  
  E \--\> F\[Inspect runtime trace\] \--\> G\[Change one property\]  
  G \--\> H\[Rebuild only\<br/\>affected outputs\] \--\> I\[Undo, save,\<br/\>reopen, recover\]  
  I \--\> J\[Package \+ verify\<br/\>provenance\] \--\> K\[Archive evidence\]  
The slice passes only if every step goes through the canonical model with correct invalidation, transactional undo, crash recovery and verifiable provenance.

### **What to do this week**

* ☐ Read section 1 and 2.7 and mark anything you disagree with as a comment.  
* ☐ Write SL-GOV-001 Constitution from section 1 (Volume 01, episodes 001–020).  
* ☐ Watch Kohi \#000–\#002, then run the prompt below for episodes 041–048.  
* ☐ Type the platform layer and logger yourself after Claude Code drafts them.

### **Starter prompt for Claude Code (episodes 041–048)**

You are working in an empty repo for Sliced Labs, a C++23 / Vulkan game engine and editor.  
Goal: complete episodes SL-EP-041 to SL-EP-048 (toolchain, repo layout, build system, lockfiles).

Constraints:  
\- C++23, Clang and GCC on Arch Linux; Windows (MSVC \+ Clang-cl) must also configure.  
\- CMake 3.28+ with Ninja; presets for debug, release, asan, ubsan, tsan.  
\- Layout: engine/{core,runtime,render,fabric}, editor/, tools/, sdk/, tests/, docs/, third\_party/, examples/.  
\- A cmake/features/ directory with compile tests proving each C++23 feature we rely on;  
  a C++26 probe list that reports but never fails the build.  
\- Vulkan SDK detected; validation layers on in debug builds.  
\- Dependencies pinned through vcpkg manifest mode with a committed baseline (lockfile).  
\- clang-format and clang-tidy configs; warnings as errors in CI presets.  
\- A tests/ target using doctest with one passing smoke test.  
\- scripts/ci-local.sh that configures, builds, runs tests and checks formatting.

Acceptance (show me the commands and their output):  
1\. Fresh clone → scripts/ci-local.sh exits 0\.  
2\. Breaking formatting in one file makes it exit non-zero.  
3\. Removing a required C++23 feature test fails configure with a clear message.  
4\. docs/episodes/041-048.md records what was done, decisions made, and open issues.

Do not write engine code beyond an empty \`sl\_core\` library and an \`sl\_sandbox\` executable.

### **Sources**

* [Kohi Game Engine repository (travisvroman/kohi)](https://github.com/travisvroman/kohi)  
* [Community index of Kohi episodes (nullshii/luna)](https://github.com/nullshii/luna)