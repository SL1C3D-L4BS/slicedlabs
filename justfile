# Sliced Labs monorepo gates (WO-001): the complete recipe surface.
# Every recipe is listed in tools/governance/recipes.json with its owner WO. A recipe that is not
# implemented yet is a stub: it prints `stub: owner WO-0xx` and exits 3. The gate ratchets through
# `fast` (below); WO-020 extended `check` (the `check: fast` line stays first).
set shell := ["bash", "-euo", "pipefail", "-c"]

default:
    @just --list

# gate.fast (FACTORY.md): under 60 s. WO-001 ratchet: layout, gates, tool tests, format, ADRs, work orders.
fast: layout-check gates-check tools-test fmt-check adr-check wo-check

# gate.full (FACTORY.md). WO-020 ratchet: fast + C++ build gates + dependency/feature/Vulkan checks + tests + every negative test + the local CI (`check-full` adds the sanitizer and release workflows).
check: fast gate-build deps-check features-check vk-check test negatives ci-local
    @echo "check: ok"

# every work order: front matter, class caps, `just <recipe>` acceptance, `Traces:` ids (in SL-REG-002 once it exists) (WO-012)
wo-check:
    @python3 -I tools/governance/cmd_wo_check.py

# ADR-0001 directories have READMEs; no tracked 0-byte files or submodules
layout-check:
    @git ls-files -z --cached --others --exclude-standard | python3 -I tools/governance/cmd_layout_check.py

# FACTORY.md parses, gate commands are recipes, .gitignore stub block, no network fetches in recipes
gates-check:
    @python3 -I tools/governance/cmd_gates_check.py

# clang-format dry run over every C++ source (passes on zero files)
fmt-check:
    @git ls-files -z --cached --others --exclude-standard -- '*.cpp' '*.hpp' '*.h' '*.cc' '*.cxx' '*.inl' | xargs -0 -r clang-format --dry-run -Werror
    @echo "fmt-check: ok"

# reformat every C++ source in place
fmt:
    @git ls-files -z --cached --others --exclude-standard -- '*.cpp' '*.hpp' '*.h' '*.cc' '*.cxx' '*.inl' | xargs -0 -r clang-format -i

# unit tests of the governance tools (stdlib unittest)
tools-test:
    @python3 -I -m unittest discover -s tools -p 'test_*.py'

# ADR front matter, file names, supersedes links, 80-line cap, ADR-0001 present, SL-GOV-004 not stale
adr-check:
    @python3 -I tools/governance/cmd_check_adr.py

# installed tools vs toolchain/environments.json; exit 1 and a `sudo pacman -S` line per missing required package (WO-013)
doctor:
    @bash scripts/doctor

# recipes.json agrees with the justfile and no gate recipe is a stub of a landed WO
factory-selftest:
    @python3 -I tools/governance/cmd_factory_selftest.py

# negative test: a planted badly formatted file must fail fmt-check and be named
neg-format:
    @bash tests/gates/neg_format.sh

# print `stub: owner WO-0xx` for a recipe from recipes.json and exit 3
stub name:
    @python3 -I tools/governance/cmd_stub.py {{name}}

# --- stubs: owner and status live in tools/governance/recipes.json ---

# schemas stay in the validator subset; the subset validator, the rule fixtures and python jsonschema (when importable) agree
registry-schema-selftest:
    @python3 -I tools/governance/cmd_registry_schema_selftest.py

# negative test: each planted registry defect (bad-<rule>.json) fails with exactly its rule id
neg-registry:
    @bash tests/gates/neg_registry.sh

# controlled documents in docs/gov, docs/arch, docs/plan, docs/vol: front matter, sections, citations, length
doc-check:
    @python3 -I tools/governance/cmd_check_docs.py

# negative test: planted bad documents must fail doc-check with the expected rule id
neg-doc:
    @bash tests/gates/neg_doc.sh

# SL-REG-002: slib rule engine plus golden asserts (840 episodes, 42 volumes of 20, 7 parts, plan 4.2 graph, lock); prints wall time
registry-check:
    @python3 -I tools/governance/cmd_check_episodes.py

# re-parse the pinned master plan and compare with SL-REG-002 (id, number, volume, title, summary, meaning_sha256)
registry-gen-check:
    @python3 -I tools/governance/cmd_gen_episodes.py --check

# episodes per lifecycle state and per volume
registry-stats:
    @python3 -I tools/governance/cmd_stats.py

# SL-REG-001 validates, equals the generator output, derived status/approvals sound, every doc file registered (WO-005)
registry-docs-check:
    @python3 -I tools/governance/cmd_check_docs_registry.py

# the 20 Volume 01 documents each have one authoring WO; episodes 001-020 each have one primary document (WO-005)
vol01-coverage:
    @python3 -I tools/governance/cmd_vol01_coverage.py

# SL-GOV-001 locked decisions and SL-GOV-006 F1-F8 sl-data blocks equal the master plan tables (WO-006)
constitution-check:
    @python3 -I tools/governance/cmd_constitution_check.py

# list sl-data contracts in docs/gov and docs/arch whose verifier is pending, a stub recipe or a missing test; never fails (WO-006, WO-035)
contracts-pending:
    @python3 -I tools/governance/cmd_contracts_pending.py

# SL-GOV-002 requirements and SL-REG-003 rows agree; episode, architecture and test references exist (WO-007)
rtm-check:
    @python3 -I tools/governance/cmd_check_rtm.py

# negative test: each planted traceability defect (dangling episode id and others) fails rtm-check with its rule id (WO-007)
neg-rtm:
    @bash tests/gates/neg_rtm.sh

# SL-REG-004 capabilities vs master plan 2.2 (12 subsystems) and 2.7 (parity rows), SL-REG-001/002 refs, SL-GOV-003/010 blocks, no parity claims (WO-008)
capability-check:
    @python3 -I tools/governance/cmd_check_caps.py

# SL-REG-006: schema valid, exactly the six master plan 3.1 classes, each exception has class, F1..F8, reason, owner, expires, review (WO-009)
exception-check:
    @python3 -I tools/governance/cmd_check_exceptions.py

# SL-GOV-008 sl-data block: contracts C01..C12, enforcers, provider orders, licence list (WO-010)
policy-contracts-check:
    @python3 -I tools/governance/cmd_policy_contracts_check.py

# SL-PLAN-002/004/005, SL-VOL-03-SPEC and SL-VOL-04-SPEC agree with SL-REG-002: episode ids, 4.3 gate episodes, graph file, 041-060 and 061-080 records (R-PLAN-VOL04: traces both ways, one primary_wo), Kohi table, clean room (WO-011, WO-035)
plan-check:
    @python3 -I tools/governance/cmd_plan_check.py

# write registry/views/ (volumes.md, volume-graph.mmd, episodes.csv, kohi-map.md) from SL-REG-002 (WO-011)
registry-views:
    @python3 -I tools/governance/cmd_views.py

# regenerate the views in memory; R-VIEW-DRIFT when a committed view differs (WO-011)
registry-views-check:
    @python3 -I tools/governance/cmd_views.py --check

# every evidence/WO-NNN.json matches its work order hash and its git objects exist; founder run, reads git (WO-012)
evidence-verify *wos:
    @bash scripts/evidence-verify {{wos}}

# evidence-record WO-NNN [gate-log] [--direct]: founder writes evidence/WO-NNN.json after the merge (WO-012)
evidence-record wo *args:
    @bash scripts/evidence-record {{wo}} {{args}}

# ep-advance <SL-EP-nnn> <state> <WO-nnn> [--adr ADR-nnnn] [--date YYYY-MM-DD]: validated 6.2 transition (WO-012)
ep-advance id state wo *flags:
    @python3 -I tools/governance/cmd_ep_advance.py {{id}} {{state}} {{wo}} {{flags}}

# negative test: with cmake absent from the tool PATH the doctor exits 1 and names cmake (WO-013)
doctor-selftest:
    @bash tests/gates/doctor_selftest.sh

# configure <preset>: cmake --preset (build/<preset>) (WO-014)
configure preset:
    @cmake --preset {{preset}}

# build <preset>: cmake --build --preset (configure first) (WO-014)
build preset:
    @cmake --build --preset {{preset}}

# test-preset <preset>: ctest --preset; noTestsAction is error, so an empty test set fails (WO-014)
test-preset preset:
    @ctest --preset {{preset}}

# workflow <preset>: cmake --workflow --preset, configure + build + test in one step (WO-014)
workflow preset:
    @cmake --workflow --preset {{preset}}

# workflow debug (clang) then gcc-debug, then the built sandbox runs its own --self-check; prints wall time (WO-014)
cpp-smoke:
    @start=$SECONDS; \
    cmake --workflow --preset debug; \
    cmake --workflow --preset gcc-debug; \
    build/debug/examples/sandbox/sl_sandbox --self-check; \
    build/gcc-debug/examples/sandbox/sl_sandbox --self-check; \
    echo "cpp-smoke: ok ($((SECONDS - start)) s wall: workflow debug, workflow gcc-debug, --self-check x2)"

# CMakePresets.json: schema 8, build+test preset per configure preset, bases, host conditions, binaryDir, no duplicate keys (WO-014)
presets-check:
    @python3 -I tools/presets/check.py

# cmake -P table test of sl_warning_flags for GNU, Clang, MSVC and clang-cl, plus the informational clang-cl probe (WO-014)
warnings-table-test:
    @cmake -P cmake/tests/warnings_table.cmake

# the C++ build gates (WO-014): cpp-smoke presets-check warnings-table-test. Not in `fast`/`check` yet: WO-020 ratchets it
gate-build: cpp-smoke presets-check warnings-table-test
    @echo "gate-build: ok"

# fresh feature cache on clang and gcc; fail only when a C++23 result differs from cmake/features/matrix.json (C++26 changes are printed) (WO-015)
features-check:
    @bash cmake/features/run.sh fresh
    @python3 -I cmake/features/features.py check

# measure every C++23 candidate on clang and gcc, write cmake/features/matrix.json and the deferred reasons; `just features-measure --apply` also rewrites the lists (WO-015)
features-measure *args:
    @bash cmake/features/run.sh fresh
    @python3 -I cmake/features/features.py measure {{args}}

# print the C++26 probe report (import std, stacktrace, reflection, ...); report only, exit 0 (WO-015)
cxx26-report:
    @bash cmake/features/run.sh cached
    @python3 -I cmake/features/features.py report

# negative test: a deleted required C++23 test file stops configure with the exact message (WO-015)
neg-feature-removed:
    @bash tests/gates/neg_feature_removed.sh

# negative test: a cxx23/*.cpp in no list stops configure and is named (WO-015)
neg-feature-unlisted:
    @bash tests/gates/neg_feature_unlisted.sh

# negative test: a C++26 probe that cannot compile never stops configure; the report marks it fail (WO-015)
neg-probe-never-fails:
    @bash tests/gates/neg_probe_never_fails.sh

# helper tests, then configure the debug preset and assert build/debug/vulkan-info.json: headers >= 350, loader found, validation on, layer api_version set; no device is opened (WO-016)
vk-check:
    @cmake -P cmake/tests/vulkan_detect.cmake
    @cmake --preset debug
    @python3 -I -c 'import json; d = json.load(open("build/debug/vulkan-info.json")); assert d["headers_version"] >= 350, d; assert d["loader_found"] is True, d; assert d["validation"] is True, d; assert d["layer_api_version"], d; print("vk-check: ok headers", d["headers_version"], "layer api", d["layer_api_version"], "slangc", (d["slangc"]["version"] or "absent"), "spirv-val", (d["spirv_val"]["path"] or "absent"))'

# advisory: vulkaninfo identity and absent-extension facts (opens a VkInstance; outside check; always exit 0) (WO-016)
vk-doctor:
    @bash scripts/vk-doctor

# negative test: ci configure with an empty SL_VK_LAYER_DIRS fails with the pacman line; the control configure exits 0 (WO-016)
neg-vk-layer-missing:
    @bash tests/gates/neg_vk_layer_missing.sh

# manifest and lock validate, hashes match the disk, third_party is covered, generated files equal the lock (WO-017); `just deps-check --strict` fails on an unpinned first provider
deps-check *args='':
    @python3 -I tools/deps/sldeps check {{args}}

# recompute deps/deps.lock.json and third_party MANIFEST.json files offline (WO-017); e.g. `just deps-lock --tested doctest=2.5.3`
deps-lock *args='':
    @python3 -I tools/deps/sldeps lock {{args}}

# write deps/vcpkg/vcpkg.json and vcpkg-configuration.json from the lock (WO-017)
deps-sync-vcpkg:
    @python3 -I tools/deps/sldeps sync-vcpkg

# copy a package's system header into third_party and recompute the lock, offline (WO-017)
deps-vendor pkg:
    @python3 -I tools/deps/sldeps vendor --from-system {{pkg}}

# SL-GOV-008 policy block equals deps/deps.lock.json policy; passes while the lock is absent (WO-010, owner WO-017)
gov008-lock-check:
    @python3 -I tools/governance/cmd_gov008_lock_check.py

# negative test: a corrupted vendored hash fails deps-check and names the file (WO-017)
neg-deps:
    @bash tests/gates/neg_deps.sh

# negative test: --strict fails and names an unpinned provider that is first in ci order (WO-017)
neg-deps-unpinned-provider:
    @bash tests/gates/neg_deps_unpinned_provider.sh

# test [name]: build and run the unit tests, or one test by name (WO-018)
test name='':
    @cmake --preset debug
    @cmake --build --preset debug
    @name={{quote(name)}}; if [ -n "$name" ]; then ctest --preset debug -R "$name"; else ctest --preset debug; fi

# deps-provider-test <provider>: vendored, system (WO-018); cpm-offline (file:// archive, accept and reject), vcpkg-fixture (fake toolchain and port) (WO-019)
deps-provider-test provider:
    @bash tests/gates/deps_provider_test.sh {{quote(provider)}}

# negative test: a corrupted vendored header is a configure error naming file and provider and never falls through; SL_DEPS_PROVIDERS=cpm lists the providers tried (WO-018)
neg-deps-missing:
    @bash tests/gates/neg_deps_missing.sh

# negative test: without vcpkg the vcpkg-debug preset and SL_DEPS_PROVIDERS=vcpkg refuse and name `sudo pacman -S vcpkg`; the control run configures (WO-019)
neg-vcpkg-absent:
    @bash tests/gates/neg_vcpkg_absent.sh

# FOUNDER ONLY, network: `SL_DEPS_ALLOW_NETWORK=1 just deps-lock-refresh <pkg>` downloads the pinned cpm archive (tools/deps/fetch_archive.sh) into build/deps-refresh/ and records its sha256 as the lock's url_hash (WO-019)
deps-lock-refresh pkg:
    @test "${SL_DEPS_ALLOW_NETWORK:-}" = 1 || { echo "deps-lock-refresh: refused: this downloads from the network and is founder-run. Re-run as: SL_DEPS_ALLOW_NETWORK=1 just deps-lock-refresh {{pkg}}" >&2; exit 2; }
    @mkdir -p build/deps-refresh
    @url="$(python3 -I tools/deps/sldeps cpm-url {{quote(pkg)}})" && echo "deps-lock-refresh: fetching $url" && bash tools/deps/fetch_archive.sh "$url" "build/deps-refresh/{{pkg}}.archive" && python3 -I tools/deps/sldeps lock-refresh {{quote(pkg)}} --file "build/deps-refresh/{{pkg}}.archive" && python3 -I tools/deps/sldeps check

# FOUNDER ONLY, network: `SL_DEPS_ALLOW_NETWORK=1 just deps-cpm-smoke` seeds a cache online (cpm-debug, SL_OFFLINE=OFF), then rebuilds and tests offline from that cache (WO-019)
deps-cpm-smoke:
    @test "${SL_DEPS_ALLOW_NETWORK:-}" = 1 || { echo "deps-cpm-smoke: refused: this downloads from the network and is founder-run. Re-run as: SL_DEPS_ALLOW_NETWORK=1 just deps-cpm-smoke (the lock needs a url_hash: SL_DEPS_ALLOW_NETWORK=1 just deps-lock-refresh doctest)" >&2; exit 2; }
    @rm -rf build/cpm-smoke build/cpm-smoke-offline
    @cmake --preset cpm-debug -B build/cpm-smoke -DSL_OFFLINE=OFF
    @cmake --preset cpm-debug -B build/cpm-smoke-offline "-DSL_CPM_SOURCE_CACHE=$PWD/build/cpm-smoke/_cpm_cache"
    @cmake --build build/cpm-smoke-offline --target sl_tests
    @ctest --test-dir build/cpm-smoke-offline -R '^sl_tests$' --no-tests=error

# FOUNDER ONLY, network: `SL_DEPS_ALLOW_NETWORK=1 just deps-vcpkg-smoke` configures vcpkg-debug (vcpkg installs deps/vcpkg), builds and runs sl_tests; needs `sudo pacman -S vcpkg` and VCPKG_ROOT (WO-019)
deps-vcpkg-smoke:
    @test "${SL_DEPS_ALLOW_NETWORK:-}" = 1 || { echo "deps-vcpkg-smoke: refused: vcpkg downloads from the network and this is founder-run. Re-run as: SL_DEPS_ALLOW_NETWORK=1 just deps-vcpkg-smoke (needs vcpkg: sudo pacman -S vcpkg, then export VCPKG_ROOT)" >&2; exit 2; }
    @test -n "${VCPKG_ROOT:-}" || { echo "deps-vcpkg-smoke: VCPKG_ROOT is unset; install vcpkg with: sudo pacman -S vcpkg, then export VCPKG_ROOT=<its root>" >&2; exit 2; }
    @cmake --preset vcpkg-debug
    @cmake --build --preset vcpkg-debug --target sl_tests
    @ctest --preset vcpkg-debug -R '^sl_tests$'

# ci-local [--quick|--full]: doctor, fmt-check, gov-check, then workflows ci-clang, ci-gcc, ci-asan (--quick: ci-clang only; --full adds ubsan, tsan, release); timed summary, exit 1 naming the first failing step (WO-020)
ci-local *args:
    @bash scripts/ci-local.sh {{args}}

# copy of the repository (git clone --no-hardlinks of HEAD once HEAD holds the tree, else the working tree) in build/fresh; ci-local under env -i with HOME in build/ and SL_DEPS_PROVIDERS=vendored; nothing outside build/ may change (WO-020)
fresh-clone-check *args:
    @bash scripts/fresh-clone-check {{args}}

# negative test: a planted failing TEST_CASE makes ci-local exit non-zero at the test step of workflow ci-clang; the control run exits 0 (WO-020)
neg-ci-local-red:
    @bash tests/gates/neg_ci_local_red.sh

# the four starter-prompt checks of episodes 041-048 (fresh clone, neg-format, feature negatives, episodes doc), logs in build/acceptance/ (WO-020)
acceptance-041-048:
    @bash scripts/acceptance-041-048

# the five infrastructure-release checks of episodes 049-060 (fresh clone, provenance, test report, ci yaml, episodes doc), logs 5-9 in build/acceptance/ (WO-034)
acceptance-049-060:
    @bash scripts/acceptance-049-060

# every negative test and selftest (WO-020)
negatives: neg-format neg-doc neg-registry neg-rtm neg-deps neg-deps-missing neg-deps-unpinned-provider neg-vcpkg-absent neg-feature-removed neg-feature-unlisted neg-probe-never-fails neg-vk-layer-missing neg-ci-local-red doctor-selftest neg-plan
    @echo "negatives: ok"

# the final `check` ratchet as a recipe (same dependency list); `check` itself is the protected gate line (WO-020)
check-ratchet: fast gate-build deps-check features-check vk-check test negatives ci-local
    @echo "check-ratchet: ok"

# release gate: check-ratchet plus the ubsan, tsan, hardened release (clang and gcc) and gcc workflows (WO-020), then the static-analysis and hardening gates tidy analyze sanitize-proof hardening neg-tidy (WO-025)
check-full: check-ratchet (workflow "ci-ubsan") (workflow "ci-tsan") (workflow "ci-release") (workflow "ci-gcc") (workflow "ci-gcc-release") tidy analyze sanitize-proof hardening neg-tidy
    @echo "check-full: ok"

# episodes-doc-check <range>: docs/episodes/<range>.md has a section per episode (registry title, five sub-headings, a just recipe, an ADR or none); e.g. 041-048 (WO-021)
episodes-doc-check range:
    @python3 -I tools/governance/cmd_episodes_doc_check.py {{quote(range)}}

# write registry/views/SL-REG-005-dependencies.md from deps/deps.lock.json (generated view, never hand-edited; WO-021)
deps-register:
    @python3 -I tools/deps/sldeps register --write

# the 20 Volume 01 documents exist with valid front matter, one authoring WO each, one primary document and WO per episode, every contract tied to a recipe or pending (WO-022)
vol01-gate:
    @python3 -I tools/governance/cmd_vol01_gate.py

# every governance gate of the registry and documents (WO-022)
gov-check: registry-check registry-docs-check doc-check adr-check wo-check rtm-check capability-check exception-check plan-check registry-views-check vol01-coverage constitution-check policy-contracts-check

# before SL-GOV-001 is approved: "not yet approved", exit 0; after: sha256, Approved status and locked rows frozen (WO-023)
constitution-frozen:
    @python3 -I tools/governance/cmd_constitution_frozen.py

# ---- stubs declared for episodes 049-060 (WO-024..WO-034); each owner replaces its body ----

# fmt-check, then tools/format/check.py --check (CMake, Python, shell, JSON, doc front matter) and shfmt -d when installed (WO-024)
fmt-check-all *args:
    @{{just_executable()}} fmt-check
    @git ls-files -z --cached --others --exclude-standard | python3 -I tools/format/check.py --check --files-from-stdin {{args}}
    @if command -v shfmt >/dev/null 2>&1; then git ls-files -z --cached --others --exclude-standard | python3 -I tools/format/check.py --list shell --files-from-stdin | xargs -0 -r shfmt -d -i 2 -ci; echo "fmt-check-all: shfmt ok"; else echo "fmt-check-all: note: shfmt is not installed, shfmt -d skipped"; fi
    @echo "fmt-check-all: ok"

# fmt, then tools/format/check.py --fix and shfmt -w when installed: rewrites what can be fixed mechanically (WO-024)
fmt-all:
    @{{just_executable()}} fmt
    @git ls-files -z --cached --others --exclude-standard | python3 -I tools/format/check.py --fix --files-from-stdin
    @if command -v shfmt >/dev/null 2>&1; then git ls-files -z --cached --others --exclude-standard | python3 -I tools/format/check.py --list shell --files-from-stdin | xargs -0 -r shfmt -w -i 2 -ci; fi

# negative test: one planted defect per kind (cmake, python, shell, json, frontmatter) fails fmt-check-all and names its kind; the control run exits 0 (WO-024)
neg-format-all *args:
    @bash tests/gates/neg_format_all.sh {{args}}

# run-clang-tidy (.clang-tidy, WarningsAsErrors '*') over the first-party sources of build/debug: engine/ examples/ tests/, minus tests/proofs and the debt listed with reasons in tests/proofs/tidy-allow.txt; extra args go to run-clang-tidy (WO-025)
tidy *args:
    @cmake --preset debug
    @cmake --build --preset debug
    @python3 -I tests/proofs/tidy_files.py build/debug/compile_commands.json tests/proofs/tidy-allow.txt >build/debug/tidy-files.txt
    @xargs -d '\n' -a build/debug/tidy-files.txt run-clang-tidy -p build/debug -quiet -j "$(nproc)" {{args}}
    @echo "tidy: ok ($(wc -l <build/debug/tidy-files.txt) files)"

# clang static analyzer (analyze-build --ctu --status-bugs, else clang-tidy clang-analyzer-*), cppcheck, and gcc -fanalyzer (gcc-debug with SL_ANALYZER=ON in build/gcc-analyzer); a finding fails it (WO-025)
analyze *args:
    @cmake --preset debug
    @cmake --build --preset debug
    @rm -rf build/analyze
    @# analyze-build is a Python script; 3.14 defaults multiprocessing to forkserver, which needs a unix socket a jail may deny: force fork
    @if command -v analyze-build >/dev/null 2>&1; then \
      lib="$(dirname "$(readlink -f "$(command -v analyze-build)")")/../lib"; \
      python3 -I -c 'import multiprocessing, sys; multiprocessing.set_start_method("fork"); sys.path.append(sys.argv.pop(1)); from libscanbuild.analyze import analyze_build; sys.exit(analyze_build())' "$lib" --cdb build/debug/compile_commands.json --ctu --status-bugs -o build/analyze {{args}}; \
    else \
      echo "analyze: analyze-build not found, falling back to clang-tidy clang-analyzer-*"; \
      python3 -I tests/proofs/tidy_files.py build/debug/compile_commands.json tests/proofs/tidy-allow.txt | xargs -d '\n' run-clang-tidy -p build/debug -quiet -j "$(nproc)" '-checks=-*,clang-analyzer-*' '-warnings-as-errors=*'; \
    fi
    @echo "analyze: clang analyzer ok"
    @cppcheck --project=build/debug/compile_commands.json --enable=warning,performance,portability --std=c++23 --error-exitcode=1 --inline-suppr --suppressions-list=.cppcheck-suppressions --quiet
    @echo "analyze: cppcheck ok"
    @cmake --preset gcc-debug -B build/gcc-analyzer -DSL_ANALYZER=ON -DSL_WARNINGS_AS_ERRORS=ON
    @cmake --build build/gcc-analyzer
    @echo "analyze: ok (clang analyzer, cppcheck, gcc -fanalyzer)"

# each sanitizer must catch its planted defect: tests/proofs asan_overflow, ubsan_shift, tsan_race built with SL_PROOFS=ON under the asan, ubsan and tsan presets (build/neg/proof-<san>) must exit non-zero and print the sanitizer's report; tsan that cannot run in a jail is marked unproven (WO-025)
sanitize-proof *args:
    @bash tests/gates/sanitize_proof.sh {{args}}

# ci-release (clang) and ci-gcc-release (gcc) with SL_HARDEN=ON: build, run the bounds-trap tests, then scripts/check-hardening reads sl_sandbox and sl_tests of each (PIE, RELRO, BIND_NOW, NX stack, CET, canary, fortify) and must reject a non-hardened debug binary (WO-025)
hardening *args:
    @cmake --preset ci-release
    @cmake --build --preset ci-release
    @ctest --preset ci-release -R 'trap_bounds|sandbox_self_check'
    @cmake --preset ci-gcc-release
    @cmake --build --preset ci-gcc-release
    @ctest --preset ci-gcc-release -R 'trap_bounds|sandbox_self_check'
    @bash scripts/check-hardening build/ci-release/examples/sandbox/sl_sandbox build/ci-release/tests/unit/sl_tests
    @bash scripts/check-hardening build/ci-gcc-release/examples/sandbox/sl_sandbox build/ci-gcc-release/tests/unit/sl_tests
    @cmake --preset debug
    @cmake --build --preset debug --target sl_sandbox
    @if bash scripts/check-hardening build/debug/examples/sandbox/sl_sandbox >build/debug/hardening-negative.log 2>&1; then echo "hardening: FAIL: a non-hardened debug binary passed check-hardening"; exit 1; fi; grep -q '^FAIL' build/debug/hardening-negative.log && echo "hardening: negative control ok ($(grep -c '^FAIL' build/debug/hardening-negative.log) properties named missing on the debug binary)"
    @echo "hardening: ok"

# negative test: a bugprone defect planted in a copy of the tree makes tidy fail and name the check; the control run exits 0 (WO-025)
neg-tidy:
    @bash tests/gates/neg_tidy.sh

# test-tiers [tier...]: configure and build debug once, then `ctest -L <tier>` per tier (default: unit integration system e2e); prints a count per tier and fails when a tier has no tests (WO-026)
test-tiers *tiers='':
    @cmake --preset debug
    @cmake --build --preset debug
    @tiers={{quote(tiers)}}; tiers="${tiers:-unit integration system e2e}"; bad=0; \
    for tier in $tiers; do \
      n=$(ctest --test-dir build/debug -N -L "^${tier}\$" | sed -n 's/^Total Tests: //p'); \
      if [ "${n:-0}" -eq 0 ]; then echo "test-tiers: FAIL: tier '$tier' has no tests"; bad=1; continue; fi; \
      echo "test-tiers: $tier: $n test(s)"; \
      ctest --test-dir build/debug -L "^${tier}\$" --no-tests=error --output-on-failure || { echo "test-tiers: FAIL: tier '$tier'"; bad=1; }; \
    done; \
    if [ "$bad" -ne 0 ]; then exit 1; fi; \
    echo "test-tiers: ok ($tiers)"

# test-report: all tests on debug; build/debug/test-report.xml (ctest JUnit) and build/debug/test-report.md (tier counts and durations) (WO-026)
test-report:
    @cmake --preset debug
    @cmake --build --preset debug
    @rc=0; ctest --test-dir build/debug --output-junit test-report.xml --output-on-failure || rc=$?; \
    python3 -I docs/testing/test_report.py build/debug; \
    exit $rc

# negative test: a unit test that sleeps 3 s with TIMEOUT 1 fails ctest and is reported as Timeout; the control run passes (WO-026)
neg-test-timeout:
    @bash tests/gates/neg_test_timeout.sh

# fixtures-check: every file under tests/fixtures matches tests/fixtures/MANIFEST.json; fails on drift or an unmanifested file (R-FIX-*) (WO-027); `python3 -I tools/testing/fixtures.py lock` rewrites the manifest
fixtures-check *args:
    @python3 -I tools/testing/fixtures.py check {{args}}

# golden-check: configure and build debug, then run the golden tests (`ctest -R golden`); SL_UPDATE_GOLDEN=1 rewrites golden files (WO-027)
golden-check *args:
    @cmake --preset debug
    @cmake --build --preset debug
    @ctest --test-dir build/debug -R golden --no-tests=error --output-on-failure {{args}}

# negative test: editing a golden file in a copy of the tree fails its test with a diff; the control run passes (WO-027)
neg-golden *args:
    @bash tests/gates/neg_golden.sh

# structural check of .github/workflows/*.yml and dependabot.yml: every `uses:` pinned to a 40-hex sha, read-only permissions, no pull_request_target, builds only via scripts/ci-local.sh or an existing preset; rules R-CI-*; placeholder shas warn (R-CI-UNVERIFIED-SHA), `--strict` fails them (WO-028)
ci-yaml-check *args:
    @python3 -I tools/ci/check_workflows.py {{args}}

# ci-local --full: the default path plus ci-ubsan, ci-tsan, ci-release, ci-gcc-release; a workflow preset missing from CMakePresets.json prints "skipped: no workflow preset" and the run still exits 0 (WO-028)
ci-local-full *args:
    @bash scripts/ci-local.sh --full {{args}}

# configure + build debug, then every artifact (sl_core, sl_sandbox, sl_tests) has a schema-valid provenance record whose artifact and input hashes and environment_hash still hold; rules R-PROV-* (WO-029, SL-GOV-006 F3)
provenance-check *args:
    @cmake --preset debug
    @cmake --build --preset debug
    @python3 -I tools/provenance/check.py {{args}}

# negative test: a source edited after the build makes provenance-check fail with R-PROV-INPUT-STALE naming it; the control run exits 0 (WO-029)
neg-provenance:
    @bash tests/gates/neg_provenance.sh

# configure + build debug, run sl_tests with SL_LOG_JSON_PATH=build/debug/log-events.jsonl, then every line validates against schemas/sl-log-event.schema.json and all six levels appear; rules R-LOG-* (WO-030, SL-GOV-006 F6)
log-schema-check *args:
    @cmake --preset debug >/dev/null
    @cmake --build --preset debug --target sl_tests
    @rm -f build/debug/log-events.jsonl
    @SL_LOG_JSON_PATH=build/debug/log-events.jsonl build/debug/tests/unit/sl_tests --minimal
    @python3 -I tools/diagnostics/log_schema_check.py --file build/debug/log-events.jsonl --require-levels {{args}}

# negative test: a planted event whose `system` is a number makes log-schema-check fail naming /system; the control run exits 0 (WO-030)
neg-log-schema *args:
    @bash tests/gates/neg_log_schema.sh

# owner WO-031
docs *args:
    @mkdir -p build/docs
    @if command -v doxygen >/dev/null 2>&1; then doxygen docs/Doxyfile && python3 -I tools/docs/build.py --api doxygen {{args}}; else python3 -I tools/docs/build.py --api stub {{args}}; fi

# owner WO-031
docs-check *args:
    @python3 -I tools/docs/check.py {{args}}

# every lock licence is on the SL-GOV-008 allowlist and every vendored dir has a manifested LICENSE (WO-032, C08)
deps-licenses *args:
    @python3 -I tools/deps/sldeps licenses {{args}}

# a vendored package is verified_upstream (deps/attestations.json) or has an unexpired waiver in deps/attestation-waivers.json (WO-032)
deps-attest-check *args:
    @python3 -I tools/deps/sldeps attest-check {{args}}

# negative test: a planted GPL licence, an unattested package, a missing LICENSE file and an open finding each fail and name the culprit (WO-032)
neg-deps-license *args:
    @bash tests/gates/neg_deps_license.sh

# dev image data: toolchain/packages.txt equals the set derived from environments.json, lock.json covers every tool, the Containerfile uses packages.txt; exit 1 on drift (WO-033)
devimage-check *args:
    @bash scripts/doctor --devimage-check {{args}}

# installed tools vs toolchain/lock.json: required drift is an error unless toolchain/drift-allow.json lists it with a reason and an until-date; optional drift warns (WO-033)
doctor-strict *args:
    @bash scripts/doctor --strict {{args}}

# ---- stubs declared for Volume 04 (WO-035..WO-063); each owner replaces its body ----

# negative test: planted SL-VOL-04-SPEC drift (primary_wo not in work_orders, acceptance absent from the primary WO, a tracing WO unlisted) fails plan-check naming R-PLAN-VOL04 and the episode; the control copy passes (WO-035)
neg-plan *args:
    @bash tests/gates/neg_plan.sh

# fabric-test <area>: configure and build preset debug, then run the CTest label fabric_<area>; an area with no tests fails (--no-tests=error); in no gate (WO-035)
fabric-test area:
    @cmake --preset debug
    @cmake --build --preset debug
    @ctest --preset debug -L {{quote("^fabric_" + area + "$")}} --no-tests=error --output-on-failure

# every FAB-INV id of SL-ARCH-001/002 has one owning WO and one test, both ways (R-FAB-*); missing tests and stub recipes are printed as pending; ends with `invariants N bound B pending P` (WO-036)
fabric-contracts-check *args:
    @python3 -I tools/fabric/contracts_check.py {{args}}

# fabric-contracts-check with pending turned into errors: every test exists and every `test: just` entry is an implemented recipe; in no gate until WO-062 (WO-036)
fabric-contracts-strict *args:
    @python3 -I tools/fabric/contracts_check.py --strict {{args}}

# negative test: a TEST_CASE naming FAB-INV-ZZ-99, a duplicate id and an invariant owned by WO-999 each fail the contracts checker naming the rule and the id; the control copy passes (WO-036)
neg-fabric-contracts *args:
    @bash tests/gates/neg_fabric_contracts.sh

# nothrow lint of engine/fabric/{src,include,gen}: N-THROW, N-TRY, N-AT, N-STO, N-FS, N-UNORDERED, N-NEW, allow-list tools/fabric/nothrow-allow.txt (ADR-0009, ADR-0016); passes while engine/fabric is absent (WO-036)
fabric-nothrow-check *args:
    @python3 -I tools/fabric/nothrow_lint.py {{args}}

# negative test: a planted throw in a copy of engine/fabric/src fails the nothrow lint naming N-THROW with file:line, and a reasonless allow entry fails naming N-ALLOW; the control copy passes (WO-036)
neg-fabric-nothrow *args:
    @bash tests/gates/neg_fabric_nothrow.sh

# fabric-golden-check: regenerate the SLB/1 corpus in memory with the Python reference and compare it with tests/fixtures/fabric/slb (schemas, accept, must-reject); drift names the file and the vector (R-GOLD-*); `python3 -I tools/fabric/corpus.py write` regenerates (WO-038)
fabric-golden-check *args:
    @python3 -I tools/fabric/corpus.py check {{args}}

# fabric-vectors-check: hashlib SHA-256, derived/well-known/imported ids, drbg blocks and object ids against tests/fixtures/fabric/hash/vectors.json (R-VEC-*); `python3 -I tools/fabric/vectors.py write` regenerates (WO-038)
fabric-vectors-check *args:
    @python3 -I tools/fabric/vectors.py check {{args}}

# negative test: a flipped hex digit in an accept vector and a changed reject offset in a copy of the corpus fail fabric-golden-check naming the file and the vector; the control copy passes (WO-038)
neg-fabric-golden *args:
    @bash tests/gates/neg_fabric_golden.sh

# fabric-schema-check: every schemas/fabric/*.sltype.json validates against sl-type-def.schema.json, the released-version lock holds, ownership against the SL-ARCH-001 domain_registry; rules R-SCH-* and R-OWN-*; zero modules passes; `python3 -I tools/fabric/schema_check.py lock` pins unreleased versions (WO-040)
fabric-schema-check *args:
    @python3 -I tools/fabric/schema_check.py check {{args}}

# negative test: editing a released field type fails fabric-schema-check naming R-SCH-RELEASED and the type, reusing a reserved field number names R-SCH-REUSE; the locked control copy passes (WO-040)
neg-fabric-schema *args:
    @bash tests/gates/neg_fabric_schema.sh

# owner WO-041
fabric-determinism *args:
    @{{just_executable()}} stub fabric-determinism

# owner WO-041
neg-fabric-determinism *args:
    @{{just_executable()}} stub neg-fabric-determinism

# owner WO-045
fabric-gen-check *args:
    @{{just_executable()}} stub fabric-gen-check

# owner WO-045
neg-fabric-gen *args:
    @{{just_executable()}} stub neg-fabric-gen

# owner WO-047
fabric-event-schema-check *args:
    @{{just_executable()}} stub fabric-event-schema-check

# owner WO-047
neg-fabric-event-schema *args:
    @{{just_executable()}} stub neg-fabric-event-schema

# owner WO-048
fabric-ownership-check *args:
    @{{just_executable()}} stub fabric-ownership-check

# owner WO-048
neg-fabric-f2 *args:
    @{{just_executable()}} stub neg-fabric-f2

# owner WO-051
fabric-surface-check *args:
    @{{just_executable()}} stub fabric-surface-check

# owner WO-051
neg-fabric-f5 *args:
    @{{just_executable()}} stub neg-fabric-f5

# owner WO-055
fabric-crash-sweep *args:
    @{{just_executable()}} stub fabric-crash-sweep

# owner WO-055
neg-fabric-crash-sweep *args:
    @{{just_executable()}} stub neg-fabric-crash-sweep

# owner WO-055
fabric-soak *args:
    @{{just_executable()}} stub fabric-soak

# owner WO-059
fabric-fsck *args:
    @{{just_executable()}} stub fabric-fsck

# owner WO-059
neg-fabric-fsck *args:
    @{{just_executable()}} stub neg-fabric-fsck

# owner WO-062
fabric-conformance *args:
    @{{just_executable()}} stub fabric-conformance

# owner WO-062
neg-fabric-conformance *args:
    @{{just_executable()}} stub neg-fabric-conformance

# owner WO-062
fabric-contract-lock *args:
    @{{just_executable()}} stub fabric-contract-lock

# owner WO-062
neg-fabric-contract-lock *args:
    @{{just_executable()}} stub neg-fabric-contract-lock

# owner WO-062
fabric-scorecard *args:
    @{{just_executable()}} stub fabric-scorecard

# owner WO-062
neg-fabric-scorecard *args:
    @{{just_executable()}} stub neg-fabric-scorecard

# owner WO-062
fabric-demo *args:
    @{{just_executable()}} stub fabric-demo

# owner WO-063
fabric-gate *args:
    @{{just_executable()}} stub fabric-gate

# owner WO-063
acceptance-061-080 *args:
    @{{just_executable()}} stub acceptance-061-080
