# docs/arch/deviations/vol05

Builder deviation records of Volume 05 (SL-ARCH-003 deviation rule; master plan section 6.3, rule 4). Builders never
edit `docs/arch/SL-ARCH-003-runtime-execution.md`; each builder work order WO-076 to WO-101 creates and commits exactly
one file here, `WO-0nn.md`, listed in its `paths_allowed`. The files are tracked (build/ is gitignored, so a record
there would be lost with the worktree). doc-check and contracts-pending scan only top-level `SL-*.md` in docs/arch, so
these files need no front matter.

One entry per decision SL-ARCH-003 left open, or per place the code departs from it:

```
## <short title>

- Decision: what the code does.
- Why: the reason (a machine fact, a test result, a missing API).
- Where: file and symbol (engine/runtime/src/<file>.cpp `<symbol>`).
- Rule or table: the ARCH3-Rnnn or sl-data table it touches, or "not covered".
```

A work order with nothing to record still commits its file with the single line `No deviations.` WO-100 reads every
file, reconciles SL-ARCH-003 to 0.2.0 (a "Code reconciliation (WO-100)" subsection with one item per entry, sl-data
tables updated to the code and bound by `just runtime-contract-lock`, open issues for what the code still has to
change) and leaves these files in place as the audit trail.
