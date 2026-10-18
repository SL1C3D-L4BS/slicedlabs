# docs/episodes

Episode records: what each episode did, decided and left open, with pointers to the evidence (master plan section 6.1).
The registry `registry/SL-REG-002-episodes.json` is authoritative (section 6.3 rule 1); these files add prose and never
change a status. `docs/templates/EPISODE.md` is the full per-episode template (WO-012).

- `041-048.md` (WO-021): SL-EP-041 to SL-EP-048, the Foundation episodes of Volume 03. One `## SL-EP-nnn — <registry
  title>` section each with `### Done`, `### Decisions`, `### Acceptance`, `### Evidence`, `### Open issues`, then the
  starter-prompt acceptance transcripts from `build/acceptance/*.log`.
- Gate: `just episodes-doc-check 041-048` (`tools/governance/cmd_episodes_doc_check.py`; rules `R-EPDOC-RANGE`,
  `R-EPDOC-FILE`, `R-EPDOC-SECTION`, `R-EPDOC-HEADINGS`, `R-EPDOC-ACCEPT`, `R-EPDOC-DECISIONS`;
  `python3 -I tools/governance/cmd_episodes_doc_check.py --selftest` plants one defect per rule).
- Episodes 001 to 020 get their records with the Volume 01 work orders.
