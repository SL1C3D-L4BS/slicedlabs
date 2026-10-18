# deps

One dependency policy, one lockfile. `deps.json` is the human-edited manifest and policy (SL-GOV-008); `deps.lock.json` is the only place a version, hash or provider pin is decided; `vcpkg/` is generated from the lock. Providers: vendored, system, cpm, vcpkg, resolved by `sl_require()` in `cmake/SlDeps.cmake`. Governed path.

Edit `deps.json`, then `just deps-lock` and `just deps-sync-vcpkg`, then `just deps-check`. Tier B tested versions: `just deps-lock --tested <pkg>=<version>`. Schemas: `schemas/sl-deps.schema.json`, `schemas/sl-deps-lock.schema.json`.
