# tools/docs

Documentation tooling (WO-031), Python 3 stdlib only, run with `python3 -I` (ADR-0004). No subprocess and no network, so
Doxygen is run by the `just docs` recipe, not by Python.

- `build.py` (`just docs`): writes `build/docs/index.md` and the pages it links (overview from the repository README, registries and
  generated views, ADR log, dependency and licence table from `deps/deps.lock.json`, records, API reference). `--api doxygen`
  when the recipe ran `doxygen docs/Doxyfile`, else `--api stub` writes a stub API page.
- `check.py` (`just docs-check`): `R-DOCS-LINK` (relative links and heading anchors in `README.md`, `docs/**/*.md`,
  `build/docs/*.md`) and `R-DOCS-ID` (cited `SL-GOV/PLAN/REG/ARCH/OPS-nnn`, `SL-VOL-nn-SPEC`, `SL-EP-nnn`, `ADR-nnnn` exist).
  Skips the verbatim master plan copy, fenced code and `docs/templates` ids. `allow-links.txt` (optional) lists accepted
  broken links as `<file>|<target>|<reason>`.
- `mdlib.py`: shared Markdown helpers. Tests: `test_docs.py` (`just tools-test`).
