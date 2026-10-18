# registry/views

Generated views of `registry/SL-REG-002-episodes.json` (master plan section 6.3, rule 1: the registry is
authoritative, views are never hand-edited). Written by `just registry-views` (`tools/governance/cmd_views.py`,
WO-011); `just registry-views-check` fails with R-VIEW-DRIFT when a file differs from the generator output.

- `volumes.md`: the 42 volumes with part, episode range and `depends_on_volumes`.
- `volume-graph.mmd`: Mermaid flowchart of the volume graph (master plan section 4.2), embedded by reference in SL-PLAN-002.
- `episodes.csv`: id, volume, title, status, kohi_ref for every episode.
- `kohi-map.md`: Kohi arcs and the Kohi-referenced episodes (master plan section 5.1), used by SL-PLAN-005.
