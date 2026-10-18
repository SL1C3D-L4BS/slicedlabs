# tools/format

The format gate for every non-C++ source kind (WO-024, episode 049). Python 3 stdlib only, `python3 -I`, no subprocess, no network (ADR-0004).

- `check.py --check|--fix [--files-from-stdin] [--list <kind>]`: kinds `cmake`, `python`, `shell`, `json`, `frontmatter`; each finding prints as `kind:path:line: message`. `--fix` rewrites trailing whitespace, final newlines, CMake tabs and JSON drift.
- `allow.txt`: paths whose owners have not fixed a finding yet (`kind path-or-glob`); the gate stays green for them only. Remove a line when its owner fixes the file.
- `test_check.py`: unit tests (`just tools-test`).
- Recipes: `just fmt-check-all` (clang-format, `check.py --check`, and `shfmt -d -i 2 -ci` when shfmt is installed), `just fmt-all` (the fixes), `just neg-format-all` (one planted defect per kind, `tests/gates/neg_format_all.sh`).
- Rules: CMake 2-space indent and no tabs; Python lines at most 120 characters and compiling; shell without tab indent; JSON equal to `json.dumps(indent=2, ensure_ascii=False)` (SL-REG-002 episodes and accepted: indent 1, sort_keys); `docs/**/SL-*.md` front matter keys in the `docs/templates/DOC.md` order.
