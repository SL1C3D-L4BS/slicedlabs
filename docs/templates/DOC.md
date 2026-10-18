---
# Controlled document (master plan section "Required documents and registries"). doc-check validates this
# front matter and the section order. Keys are one per line; lists are flow lists [a, b].
id: SL-GOV-000          # SL-<KIND>-<NNN>, or SL-VOL-NN-SPEC; the file is <id>.md or <id>-<slug>.md
kind: GOV               # GOV | ARCH | PLAN | OPS | REG | VOL (must match the id)
title: "Document title"
status: Draft           # Draft | InReview | Approved | Superseded
version: 0.1.0          # major.minor.patch
owner: founder
episodes: []            # episode numbers that write or consume this document, e.g. [020]
depends_on: []          # document ids that must be Approved first, e.g. [SL-GOV-001]
supersedes: []          # document ids this replaces
decisions: []           # ADR ids that bind this document, e.g. [ADR-0001]
# max_lines: 150        # optional: raise (or lower) the default 150-line limit
---
# Document title

## Purpose

One paragraph: what this document controls and why it exists, citing the master plan section it implements.

## Status and decisions

Current status, the ADRs and locked decisions that bind it, and who approves it.

## Normative text

Rules carry an id `<DOC>-R<NN>` (for example `GOV1-R01`) and use MUST, SHOULD or MAY (RFC 2119 sense).

- GOV0-R01: Example rule. A conforming change MUST cite its source (master plan section 6.3).

## Measures and enforcement points

For each rule: the numerator, denominator and threshold when it is measurable, and the gate (`just <recipe>`),
test or review that enforces it. A rule with no enforcement point is listed under Open issues.

## Traceability

Master plan sections, episodes (SL-EP-nnn), requirements and tests this document traces to.

## Open issues

Anything stated without a source, unverified, or deferred. A sentence that cannot cite a source belongs here.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | YYYY-MM-DD | First draft. |

<!--
Citation rule (R-DOC-CITE). Every line that contains MUST, SHOULD or MAY must also contain a citation: a
section sign, the word "section", the words "master plan", or an ADR id (ADR-NNNN). A normative sentence
with no source is not a rule; move it to Open issues.

Data blocks. Machine-readable content (tables the registry or a check compares with the plan) goes in a fenced
block with the info string sl-data holding one JSON value:

    ```sl-data
    {"rows": []}
    ```

Tools read these blocks, prose is never parsed for data. Lines inside fenced blocks are not citation-checked.
-->
