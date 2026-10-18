# toolchain

Development environment as data: `environments.json` (supported environments, tool tiers, min/tested versions, Arch
packages; WO-013, episodes 041/042), `lock.json` (exact versions installed on the reference workstation, written by
`scripts/doctor --write-lock`), `drift-allow.json` (temporary exceptions with an until-date) and `packages.txt` (the Arch
packages of the dev image; WO-033, episode 059). Checked by `just doctor`, `just doctor-strict`, `just devimage-check`.
