# CI and quality gates

Seed for SL-OPS-001 (CI and Quality Gates, Planned). Master plan episodes SL-EP-053 (local CI runner) and SL-EP-054
(initial CI pipeline: clean builds, caching, artifacts, failure reports); Windows evidence per
[ADR-0006](../adr/ADR-0006-windows-parity.md); work orders WO-028 and WO-130. The workflows could not be run when they were
written (no GitHub here): they are validated structurally and by mirroring the local runner.

## Gates

| Where | Command | Runs |
| :---- | :---- | :---- |
| Local | `just ci-local` (`--quick`, `--full`) | [`scripts/ci-local.sh`](../../scripts/ci-local.sh): doctor, fmt-check, gov-check, then workflows `ci-clang`, `ci-gcc`, `ci-asan` |
| Local | `just ci-local-full` | the same plus `ci-ubsan`, `ci-tsan`, `ci-release`, `ci-gcc-release` |
| Local | `just ci-yaml-check` | [`tools/ci/check_workflows.py`](../../tools/ci/check_workflows.py) on `.github/` |
| Hosted | [`ci.yml`](../../.github/workflows/ci.yml), job `linux` (ubuntu-24.04 host, Arch Linux container) | `bash scripts/ci-local.sh`, on push, pull_request and workflow_dispatch |
| Hosted | [`windows.yml`](../../.github/workflows/windows.yml), job `windows` (windows-latest) | installs the pinned Vulkan SDK, then configure only: `cmake --preset win-msvc-debug`, `cmake --preset win-clangcl-debug` |

The hosted Linux job is the local runner on a clean machine with the primary toolchain (see the next section), so a red hosted run reproduces with `just ci-local`.
Missing workflow presets are skipped with a visible `skipped: no workflow preset <name>` line; today the presets
`ci-ubsan`, `ci-tsan`, `ci-release` and `ci-gcc-release` are not defined in `CMakePresets.json`, so `--full` runs the
default steps and lists those four as skipped.

## The hosted Linux runner model (WO-130)

The first hosted run (job `linux`, ubuntu-24.04) failed at configure: Ubuntu's libstdc++-14 has no `flat_map`, `flat_set`,
`mdspan` or `ranges::to`, so the C++23 feature battery refused the toolchain (master plan 1.5, ADR-0005). The battery was
right and the runner was wrong, so the job now runs the primary toolchain (`toolchain/environments.json`: Arch Linux,
clang 23, gcc 16 with its libstdc++):

- The job keeps `runs-on: ubuntu-24.04` and sets `container: ghcr.io/archlinux/archlinux:base-devel@sha256:<digest>`, the
  official Arch Linux image published by the Arch Linux organisation. It is pinned by tag and digest, so the base image
  never changes under a commit. ghcr.io is used instead of Docker Hub because the digest could be resolved from it and
  hosted runners pull from it without Docker Hub rate limits; the Docker Hub image has different digests.
- Step 1 writes `/etc/pacman.d/mirrorlist` as `Server = https://archive.archlinux.org/repos/<YYYY/MM/DD>/$repo/os/$arch`
  from the job env `ARCHIVE_DATE` and runs `pacman -Syyuu` (the double `y` and `u` also downgrade a newer base image to the
  snapshot), then installs `git`, `python` and `zstd` so that checkout and the cache action work.
- After checkout, `tools/ci/check_toolchain.py packages` lists the packages in
  [`toolchain/ci-linux.json`](../../toolchain/ci-linux.json), pacman installs them, and `check_toolchain.py verify`
  fails the job when an installed version differs from the pinned one (clang, gcc, libstdc++ as `gcc-libs`, cmake, ninja,
  just, mold, ccache, shellcheck, python and the Vulkan headers, loader and validation layers, which the configure of the
  `ci-*` presets requires).
- `pacman --disable-sandbox` is used because the download sandbox (Landlock) is not reliably available inside a container.
- The ccache key includes the hash of `toolchain/ci-linux.json`, so a new snapshot starts a new cache.
- Permissions stay least-privilege: top-level `permissions: {}` and `contents: read` on the job; checkout keeps
  `persist-credentials: false`; every action is pinned by commit sha.

Unverified until a hosted run passes: the container start on GitHub, and every step after configure (build, test, ASan
inside the container). A red step is a follow-up work order, not a reason to loosen the pins.

### Bumping the snapshot

1. Pick a date that has a complete archive snapshot, normally yesterday (UTC): both
   `https://archive.archlinux.org/repos/<YYYY/MM/DD>/core/os/x86_64/core.db` and `.../extra/os/x86_64/extra.db` must
   answer 200.
2. Read the package versions on that date from the two `.db` files (a tar of `<name>-<version>-<release>/desc` entries) or
   from a machine at that date, and put them into `packages` in `toolchain/ci-linux.json` with `archive.date` and
   `archive.server`. Versions are `pacman -Q` strings, including any epoch (`1:1.4.363.0-1`).
3. Optionally refresh the image: the registry API or `docker buildx imagetools inspect
   ghcr.io/archlinux/archlinux:base-devel` gives the new digest; put it into `image.digest` and `image.created`.
4. Set the same image digest and `ARCHIVE_DATE` in `.github/workflows/ci.yml`. `just ci-yaml-check --strict` fails with
   `R-CI-TOOLCHAIN` until the workflow and the pin file agree.
5. Compare with `toolchain/environments.json` (`tested`) and `toolchain/lock.json`: the hosted pin does not need the
   workstation to match, but a difference means CI and local results can differ.

## Rules of `just ci-yaml-check`

`R-CI-USES-UNPINNED` (every `uses:` is a 40-hex commit sha), `R-CI-USES-COMMENT` (with a `# vX.Y.Z` comment),
`R-CI-UNVERIFIED-SHA` (warning, error under `--strict`: a placeholder sha or tarball sha256), `R-CI-PERMISSIONS`
(top-level `permissions` present and read-only), `R-CI-TRIGGER` (no `pull_request_target`, `workflow_dispatch` present),
`R-CI-JOB` (job ids `linux`/`windows`, runners `ubuntu-24.04`/`windows-latest`), `R-CI-CONTAINER` (a job `container:`
image is pinned by tag and `@sha256:<64 hex>`), `R-CI-TOOLCHAIN` (the container image and the job env `ARCHIVE_DATE` equal
`toolchain/ci-linux.json`, which must be well formed), `R-CI-BUILD-ENTRY` (a run step builds
only through `scripts/ci-local.sh`, `cmake --preset <configure preset>` or `cmake --workflow --preset <workflow
preset>`, read from `CMakePresets.json`), `R-CI-INJECT`, `R-CI-SECRET`, `R-CI-DEPENDABOT`, `R-CI-PARSE`. The YAML
reader is `tools/ci/miniyaml.py`, a stdlib subset (no anchors or multi-line plain scalars).

## Artifacts and where they land

| Artifact | Contents | Source path on the runner |
| :---- | :---- | :---- |
| `acceptance` | acceptance logs, when a run produced them | `build/acceptance` |
| `test-report` | ctest JUnit report, when `just test-report` ran | `build/debug/test-report.xml` |
| `ci-logs` | one log per ci-local step, ctest `LastTest.log` | `build/ci-local/*.log`, `build/*/Testing/Temporary/LastTest.log` |
| `windows-cmake-logs` | `CMakeConfigureLog.yaml`, `CMakeOutput.log`, `CMakeError.log`, `CMakeCache.txt`, `vulkan-info.json` per Windows preset | `build/win-*/` |

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
developer shell and does not build. A run summary table lists the pin check, both configure outcomes and the Vulkan facts
(WO-131). The Windows-only changes and the known Windows gaps are in the Proposed amendment to ADR-0006.

## Resolving the pins

The action shas (and any tarball sha256 values) in the workflows were placeholders (zeros, marked `UNVERIFIED`)
until they were resolved. A placeholder fails closed: the action does not resolve and `sha256sum --check` fails.
`ci.yml` no longer downloads tarballs (pacman verifies signed packages), so only action shas remain. To replace a
placeholder or bump a pin, keep the `# vX.Y.Z` comment:

- action sha: `gh api repos/<owner>/<repo>/commits/<tag> --jq .sha` (dereferences annotated tags), for
  `actions/checkout`, `actions/cache`, `actions/upload-artifact`, `ilammy/msvc-dev-cmd`;
- tarball sha256 (only where a workflow still downloads a release file): run `sha256sum` on it, or copy the checksum
  published with the release, then confirm the version against `toolchain/environments.json`.

`just ci-yaml-check --strict` must then pass with zero warnings. `.github/dependabot.yml` keeps the shas current monthly.

## Resolving the Vulkan SDK pin

`windows.yml` installs the LunarG Windows Vulkan SDK from `sdk.lunarg.com` (WO-131). `VULKAN_SDK_VERSION` and
`VULKAN_SDK_SHA256` in its top-level `env` are placeholders (an unconfirmed version, an all-zero sha256) because the
LunarG host could not be reached when they were written. Until both are resolved the `Vulkan SDK pin check` step fails
with an error naming this section, and nothing is downloaded or run. To resolve:

- pick a SDK version whose headers are at least `SL_VK_MIN_HEADER` (350); the Linux headers measured 363;
- fetch `https://vulkan.lunarg.com/sdk/sha/<version>/windows/vulkansdk-windows-X64-<version>.exe.json` and copy its
  sha256 (or download the installer and run `sha256sum`), and confirm the download URL
  `https://sdk.lunarg.com/sdk/download/<version>/windows/vulkansdk-windows-X64-<version>.exe` returns that file;
- write both into `windows.yml`, drop the `UNVERIFIED` words, and run `just ci-yaml-check --strict` (zero warnings).

The installer command line (`--root`, `--accept-licenses`, `--default-answer`, `--confirm-command install
com.lunarg.vulkan.core`) is the Qt installer framework form and has not run on a runner yet; the first hosted run
confirms it. The install directory is cached under a key made of the version and sha256.

## Known gaps

- The actions pinned in `ci.yml` (checkout 4.2.2, cache 4.2.0, upload-artifact 4.6.0) run on Node 20, which GitHub is
  deprecating; the Dependabot pull requests for checkout 7.0.1, cache 6.1.0 and upload-artifact 7.0.2 are the fix.
- The hosted `linux` job runs the default path only; the sanitizer and release workflows wait for their presets.
