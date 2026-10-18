# Development image and toolchain lock

Seed for the SL-OPS documents (SL-OPS-001 CI and Quality Gates, SL-OPS-002 Dependency and Supply-Chain Operations;
both Planned). Master plan episodes SL-EP-059 (Reproducible development image) and SL-EP-041 (Select supported
development environments); work order WO-033.

## Status: the image is unbuilt here

No image has been built on this machine, no container runtime is used by any gate, and the base image digest is not
recorded. Everything below is data and a script the founder runs. `just devimage-check` and `just doctor-strict`
are the only gates; neither starts a container or touches the network.

## Policy

The workstation follows the toolchain policy in `~/.dotfiles/docs/spec/toolchain.md`: edge OS (Arch is rolling and
tracks the newest release of everything), projects pin their own toolchains. This repository pins by recording, not
by freezing the host: `toolchain/lock.json` states the exact versions that the gates last passed with, and drift is
made visible instead of silent.

## Files

| File | Role |
| :---- | :---- |
| `toolchain/environments.json` | Tools, tiers, minimum versions, `arch_package` (WO-013). The single source. |
| `toolchain/lock.json` | Exact installed versions, written by `scripts/doctor --write-lock`. Schema `sl-toolchain-lock/1`: `written`, `host` (`os`, `kernel`), `tools` (`version`, `arch_package`, `tier`). |
| `toolchain/drift-allow.json` | Temporary allowances: `tool`, `reason`, `until` (YYYY-MM-DD, inclusive). Empty by default. |
| `toolchain/packages.txt` | One Arch package per line: the `arch_package` fields, deduplicated, sorted. |
| `scripts/dev-image/Containerfile` | `archlinux:base-devel`, `pacman -Syu --needed --noconfirm $(cat /tmp/packages.txt)`, a non-root user, a `just doctor-strict` smoke step. |
| `scripts/dev-image/build.sh` | Refuses without podman (prints the build command); with podman builds `localhost/slicedlabs-dev:<first 12 hex of sha256 of lock.json>`. |

## Gates

- `just doctor` (WO-013): installed tools against minimum versions; a missing required tool fails.
- `just doctor-strict`: every required tool must equal its version in `toolchain/lock.json`. A difference is an error
  unless `toolchain/drift-allow.json` lists the tool with a reason and an `until` date that has not passed. Optional
  tools only warn.
- `just devimage-check`: `packages.txt` equals the set derived from `environments.json`; `lock.json` validates and
  holds every tool in `environments.json` (matching `arch_package` and `tier`); the Containerfile references
  `packages.txt` and the `doctor-strict` smoke step; `build.sh` exists. Exit 1 on any drift.

## Routine

1. After a deliberate update (`sudo pacman -Syu`), run `just doctor`, then `scripts/doctor --write-lock`, review the
   `lock.json` diff and commit it with the update.
2. After editing `environments.json`, regenerate `packages.txt` (sorted unique `arch_package` values) and re-run
   `just devimage-check`.
3. To build the image (needs podman and the network): `scripts/dev-image/build.sh`. Record the base digest first
   (`podman inspect --format '{{.Digest}}' docker.io/library/archlinux:base-devel`) and pass it as
   `SL_DEV_IMAGE_BASE=archlinux@sha256:<digest>`; the comment in the Containerfile marks it unrecorded today.
4. The image installs the newest packages (Arch has no old versions), so its smoke step fails when the repositories
   moved past the lock. That is the signal to refresh the lock, not to pin old packages.

Open: the Windows environment is not covered by the lock or the image (ADR-0006).
