# docs

`plan/` (the master plan, byte copy pinned by sha256), `gov/` (SL-GOV governance documents), `vol/`
(SL-VOL volume specs), `adr/` (decisions of this repo), `episodes/` (per-episode notes), `templates/`.

- [arch/](arch/README.md): SL-ARCH-001 to SL-ARCH-030, all Planned under [ADR-0003](adr/ADR-0003-authoring-order-waivers.md); target volumes from SL-REG-001.
- [ops/](ops/README.md): SL-OPS-001 to SL-OPS-010 (Planned) and how the documentation is built and checked.
- [build/](build/README.md): the build system notes.
- [Doxyfile](Doxyfile): Doxygen configuration for the public headers in `engine/core/include`.

## Generated documentation

`just docs` writes `build/docs/index.md` and its pages from the registries (`registry/`), the ADR log
([SL-GOV-004](gov/SL-GOV-004-adr-log.md)), `deps/deps.lock.json` and the code. It runs `doxygen docs/Doxyfile` first when `doxygen`
is installed (HTML in `build/docs/api/html`), else the API page is a stub. `just docs-check` fails with `R-DOCS-LINK` for a
relative Markdown link or heading anchor that does not resolve (in `README.md`, `docs/**/*.md`, `build/docs/*.md`) and with
`R-DOCS-ID` for a cited `SL-GOV/PLAN/REG/ARCH/OPS-nnn`, `SL-VOL-nn-SPEC`, `SL-EP-nnn` or `ADR-nnnn` that does not exist. Tools:
`tools/docs/` (WO-031).
