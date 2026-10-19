# docs/episodes

Episode records: what each episode did, decided and left open, with pointers to the evidence (master plan section 6.1).
The registry `registry/SL-REG-002-episodes.json` is authoritative (section 6.3 rule 1); these files add prose and never
change a status. `docs/templates/EPISODE.md` is the full per-episode template (WO-012).

- `041-048.md` (WO-021): SL-EP-041 to SL-EP-048, the Foundation episodes of Volume 03. One `## SL-EP-nnn — <registry
  title>` section each with `### Done`, `### Decisions`, `### Acceptance`, `### Evidence`, `### Open issues`, then the
  starter-prompt acceptance transcripts from `build/acceptance/*.log`.
- `049-060.md` (WO-034): SL-EP-049 to SL-EP-060, the infrastructure episodes (formatting, static analysis, test tiers,
  fixtures, local and hosted CI, provenance, diagnostics, docs, dependency ingestion, development image, release gate), in
  the same five sub-headings, then the acceptance transcripts of `just acceptance-049-060` (`build/acceptance/5-*.log` to
  `9-*.log`).
- `061-080.md` (WO-063): SL-EP-061 to SL-EP-080, the Fabric (identity, types, schema versioning, manifest, ownership,
  provenance, serialization, events, transactions, undo, dependency graph, impact, boundary, resolver, recovery,
  corruption, generated tooling, conformance, release), in the same five sub-headings, with honest limits (simulated
  durability, Windows unverified, big-endian untested, wall time unmeasured, seams S1-S16), then the acceptance of
  `just acceptance-061-080` (`build/acceptance/10-*.log` to `12-*.log`).
- Gate: `just episodes-doc-check 041-048`, `just episodes-doc-check 049-060` and `just episodes-doc-check 061-080` (`tools/governance/cmd_episodes_doc_check.py`; rules `R-EPDOC-RANGE`,
  `R-EPDOC-FILE`, `R-EPDOC-SECTION`, `R-EPDOC-HEADINGS`, `R-EPDOC-ACCEPT`, `R-EPDOC-DECISIONS`;
  `python3 -I tools/governance/cmd_episodes_doc_check.py --selftest` plants one defect per rule).
- Episodes 001 to 020 get their records with the Volume 01 work orders.
