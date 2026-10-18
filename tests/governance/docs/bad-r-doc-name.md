---
id: SL-GOV-901
kind: GOV
title: "Fixture document"
status: Draft
version: 0.1.0
owner: founder
episodes: [020]
depends_on: []
supersedes: []
decisions: [ADR-0001]
---
# Fixture document

## Purpose

A well-formed fixture for doc-check (master plan section 6.3).

## Status and decisions

Draft. Bound by ADR-0001.

## Normative text

- GOV900-R01: A change MUST cite its source (master plan section 6.3).
- Lines inside a fence are not checked:

```sl-data
{"note": "a record MUST be valid"}
```

## Measures and enforcement points

`just doc-check`.

## Traceability

Master plan section 6.

## Open issues

None.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | Fixture. |
