# tools/deps

`sldeps`: the dependency lock tool (WO-017, ADR-0002). Run `python3 -I tools/deps/sldeps <command>`; stdlib only, offline.
Commands: `check [--strict]` (`just deps-check`), `lock` (`just deps-lock`), `sync-vcpkg` (`just deps-sync-vcpkg`),
`vendor --from-system <pkg>` (`just deps-vendor <pkg>`), `register [--write]`,
`cpm-url <pkg>` and `lock-refresh <pkg> --file <archive>` (WO-019; used by the founder recipe `SL_DEPS_ALLOW_NETWORK=1 just deps-lock-refresh <pkg>`: curl downloads, sldeps only hashes the file). Library: `sldeps_lib.py`; tests: `test_sldeps.py`.
`pending` and `unpinned` are derived at check time from the lock, never stored. `--strict` fails when a provider that is first
in ci order is unpinned.
