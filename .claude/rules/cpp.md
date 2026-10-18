---
paths:
  - "engine/**"
  - "editor/**"
  - "sdk/**"
  - "platform/**"
  - "examples/**"
  - "tests/unit/**"
  - "**/*.{cpp,hpp,h,cc,cxx,inl}"
---
# C++ rules

- C++23 is the floor (ADR-0005, master plan 1.5): `CXX_STANDARD 23`, `CXX_STANDARD_REQUIRED ON`,
  `CXX_EXTENSIONS OFF`. No GNU extensions, no compiler-specific keywords outside `platform/`.
- RAII for every resource (memory, handles, locks, GPU objects). No naked `new`/`delete`, no owning raw
  pointers, no manual cleanup paths.
- No copied code: nothing from Kohi or any other engine is read into or pasted into this repo. `~/src` is
  behavioural reference only; write from the spec (master plan 5.1 states the clean-room rule).
- Warnings are errors in CI (`SL_WARNINGS_AS_ERRORS`, ci presets). Do not silence a warning to pass; fix it,
  or add a scoped `NOLINT(check)` with a one-line reason.
- Format with `just fmt`, gate with `just fmt-check`; style lives in `.clang-format` and `.clang-tidy`
  (gate configs: change only with `touches_gates: true`).
- A new directory under `engine/`, `editor/`, `sdk/`, `platform/` gets a `README.md` (ADR-0001 layout).
- Dependencies go through the one policy (ADR-0002, `sl_require`); no ad-hoc `find_package` or fetches.
- Tests: doctest under `tests/unit/`; a bug fix lands with the test that failed first.
