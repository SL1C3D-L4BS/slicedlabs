# tests/gates

Negative tests for the gates (`neg_*.sh`): each plants a defect in a throwaway copy of the tree, runs the
gate as a control first, then requires the gate to fail and name the culprit. Governed path (FACTORY.md).
Now: `neg_format.sh` (`just neg-format`).
Also: `neg_doc.sh` (`just neg-doc`), `neg_registry.sh` (`just neg-registry`), `doctor_selftest.sh` (`just doctor-selftest`).
