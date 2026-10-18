# CI and quality gates

Seed for SL-OPS-001 (CI and Quality Gates, Planned). Master plan episodes SL-EP-053 (local CI runner) and SL-EP-054
(initial CI pipeline: clean builds, caching, artifacts, failure reports); Windows evidence per
[ADR-0006](../adr/ADR-0006-windows-parity.md); work order WO-028. The workflows could not be run when they were
written (no GitHub here): they are validated structurally and by mirroring the local runner.

## Gates

| Where | Command | Runs |
| :---- | :---- | :---- |
| Local | `just ci-local` (`--quick`, `--full`) | [`scripts/ci-local.sh`](../../scripts/ci-local.sh): doctor, fmt-check, gov-check, then workflows `ci-clang`, `ci-gcc`, `ci-asan` |
| Local | `just ci-local-full` | the same plus `ci-ubsan`, `ci-tsan`, `ci-release`, `ci-gcc-release` |
| Local | `just ci-yaml-check` | [`tools/ci/check_workflows.py`](../../tools/ci/check_workflows.py) on `.github/` |
| Hosted | [`ci.yml`](../../.github/workflows/ci.yml), job `linux` (ubuntu-24.04) | `bash scripts/ci-local.sh`, on push, pull_request and workflow_dispatch |
| Hosted | [`windows.yml`](../../.github/workflows/windows.yml), job `windows` (windows-latest) | configure only: `cmake --preset win-msvc-debug`, `cmake --preset win-clangcl-debug` |

The hosted Linux job is the local runner on a clean machine, so a red hosted run reproduces with `just ci-local`.
Missing workflow presets are skipped with a visible `skipped: no workflow preset <name>` line; today the presets
`ci-ubsan`, `ci-tsan`, `ci-release` and `ci-gcc-release` are not defined in `CMakePresets.json`, so `--full` runs the
default steps and lists those four as skipped.

## Rules of `just ci-yaml-check`

`R-CI-USES-UNPINNED` (every `uses:` is a 40-hex commit sha), `R-CI-USES-COMMENT` (with a `# vX.Y.Z` comment),
`R-CI-UNVERIFIED-SHA` (warning, error under `--strict`: a placeholder sha or tarball sha256), `R-CI-PERMISSIONS`
(top-level `permissions` present and read-only), `R-CI-TRIGGER` (no `pull_request_target`, `workflow_dispatch` present),
`R-CI-JOB` (job ids `linux`/`windows`, runners `ubuntu-24.04`/`windows-latest`), `R-CI-BUILD-ENTRY` (a run step builds
only through `scripts/ci-local.sh`, `cmake --preset <configure preset>` or `cmake --workflow --preset <workflow
preset>`, read from `CMakePresets.json`), `R-CI-INJECT`, `R-CI-SECRET`, `R-CI-DEPENDABOT`, `R-CI-PARSE`. The YAML
reader is `tools/ci/miniyaml.py`, a stdlib subset (no anchors or multi-line plain scalars).

## Artifacts and where they land

| Artifact | Contents | Source path on the runner |
| :---- | :---- | :---- |
| `acceptance` | acceptance logs, when a run produced them | `build/acceptance` |
| `test-report` | ctest JUnit report, when `just test-report` ran | `build/debug/test-report.xml` |
| `ci-logs` | one log per ci-local step, ctest `LastTest.log` | `build/ci-local/*.log`, `build/*/Testing/Temporary/LastTest.log` |
| `windows-cmake-logs` | `CMakeConfigureLog.yaml`, `CMakeOutput.log`, `CMakeError.log`, `CMakeCache.txt` per Windows preset | `build/win-*/` |

On failure the `Failure summary` step appends the tail of the last step log to the run summary. Locally the same logs
are in `build/ci-local/`.

## How to re-run

- Locally: `just ci-local` (add `--full` or `--quick`); failed step and log tail are printed.
- Hosted Linux: push, or Actions, `ci`, Run workflow. A branch re-run cancels the older run of the same ref.
- Hosted Windows: Actions, `windows`, Run workflow.

## The Windows evidence route

The structural checks on Linux (presets check, flag-table tests, clang-cl probe, portability lint) are not a Windows
configure and are never reported as one (ADR-0006). The only route to real evidence is the `windows` job on a
windows-latest runner, started by the founder after the first push. Until it has passed, the 041-048 episode notes list
Windows as unverified and no Windows support claim is made. The job configures with Ninja inside the Visual Studio
developer shell and does not build.

## Resolving the pins

The action shas and the tool tarball sha256 values in the workflows are placeholders (zeros, marked `UNVERIFIED`),
because they could not be looked up offline. They fail closed: the action does not resolve and `sha256sum --check`
fails. Before the first push, the founder resolves each one and replaces the placeholder, keeping the `# vX.Y.Z` comment:

- action sha: `gh api repos/<owner>/<repo>/commits/<tag> --jq .sha` (dereferences annotated tags), for
  `actions/checkout`, `actions/cache`, `actions/upload-artifact`, `ilammy/msvc-dev-cmd`;
- tarball sha256: download the release file named in `ci.yml` (cmake, LLVM, just) and run `sha256sum`, or copy the
  checksum published with the release, then confirm the version against `toolchain/environments.json`.

`just ci-yaml-check --strict` must then pass with zero warnings. `.github/dependabot.yml` keeps the shas current monthly.

## Known gaps

- apt provides gcc 14 on ubuntu-24.04, below the gcc 16 floor in `toolchain/environments.json`; the doctor reports
  drift, which only warns. Raise the floor handling or add a newer toolchain source when the code needs gcc 16.
- The hosted `linux` job runs the default path only; the sanitizer and release workflows wait for their presets.
