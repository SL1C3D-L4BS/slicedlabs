# tools/runtime

Volume 05 guardrails (WO-077, SL-ARCH-003). Python 3 standard library only, run as `python3 -I`, no subprocess and no
network (ADR-0004). Every script under scripts/runtime/ that runs them writes only under build/.

## runtime_lint.py (`just runtime-lint`)

`python3 -I tools/runtime/runtime_lint.py [--root DIR] [--allow FILE]` scans engine/runtime/** (comments, string literals
and include paths blanked first), the CMake files of engine/runtime/tools and scripts/runtime/*.sh. Every finding is
`<path>:<line>: <RULE>: <message>`; the exit status is 1 on any finding.

| Rule | Refuses | SL-ARCH-003 |
| --- | --- | --- |
| R-RT-LAYER | a header including a higher layer of the `layers` table; a same-layer cycle | R009 |
| R-RT-FABRIC | sl/fabric includes outside the scoped lists; vfs.hpp outside src/platform_files*.cpp | R004, R005 |
| R-RT-TXCALL | transaction_manager, transaction, workspace, tx_options under include/ and src/ | R006 |
| R-RT-MATH | vec*, mat*, quat*, transform*, fixed*, simd*, aabb*, curve* types; intrinsics headers | R007 |
| R-RT-GLOBAL | mutable namespace-scope, class-static, function-static or thread_local variables | R008 |
| R-RT-FENCE | atomic_thread_fence, atomic_signal_fence, volatile, asm, SYS_futex, syscall | R057 |
| R-RT-ORDER | a weak memory_order without `// pairs: <symbol>` on the same or previous line | R058 |
| R-RT-DIST | std distributions, random_device, rand, srand, std::hash, range-for over unordered containers | R087 |
| R-RT-PLATFORM | ::now(), std::thread, fopen, ::open, std::filesystem, file streams outside src/platform_*.cpp | R035 |
| R-RT-THROW | throw, try, catch, `.at(`, std::sto* | R010 |
| R-RT-NOEXCEPT | a lambda without noexcept passed to a job, system, task, invoker or handler | R010 |
| R-RT-LINK | engine/runtime/tools linking sl_fabric_testing | R001 |
| R-RT-OUTPUT | a runtime script writing outside build/ | R015 |
| R-RT-ALLOW | a malformed lint_allow.json | R008 |

`lint_allow.json` is `{"allow": [{"path", "rule", "symbol", "reason"}]}`; an entry excuses the findings of its path, rule
and symbol (`*` for every symbol), and an entry without a reason is an error. It holds the failpoint registry of
engine/runtime/src/faults.cpp only. The deviations and heuristics are in docs/arch/deviations/vol05/WO-077.md.

`just neg-runtime-lint` (tests/gates/neg_runtime_lint.sh) plants one defect per rule in a copy under
build/runtime/neg/lint and requires the rule id and file:line in the output.

## contracts_check.py (`just runtime-contracts`, `just runtime-contracts-strict`)

`python3 -I tools/runtime/contracts_check.py [--strict] [--root DIR]` binds every RT-INV id of the single `invariants`
sl-data block of docs/arch/SL-ARCH-003-*.md to one test file and one owner work order, both ways. C++ cases are
`TEST_CASE("RT-INV-<AREA>-<nn>: ...")` in tests/{unit,integration}/runtime/**/*.cpp, Python tests are
`def test_rt_inv_<area>_<nn>_*` in tools/runtime/test_*.py.

Rules: R-RT-SHAPE, R-RT-DUP, R-RT-RULE, R-RT-WO, R-RT-ORPHAN-TEST, R-RT-LOCATION, R-RT-AREA, R-RT-PER-WO (at most 8 rows
per owner), R-RT-RECIPE (verified_by must be a justfile recipe; a stub recipe, or a dispatch recipe whose
scripts/runtime/<name>.sh is still the two-line cmd_stub.py stub, is pending), R-RT-MISSING-TEST (pending). The default
mode prints pending items and exits 0; `--strict` turns every pending item into an error. The output ends with
`invariants N bound B pending P`.

`just neg-runtime-contracts` (tests/gates/neg_runtime_contracts.sh) deletes one invariant case, adds an orphan test and a
test in the wrong file in a copy under build/runtime/neg/contracts and requires the rule and the id.

## Tests

test_runtime_lint.py (RT-INV-LINT-01, RT-INV-LINT-02) and test_contracts_check.py (RT-INV-LINT-03, RT-INV-LINT-04) are
unittest modules run by `just tools-test`; their fixtures are temp trees built from strings.
