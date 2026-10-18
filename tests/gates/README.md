# tests/gates

Negative tests for the gates (`neg_*.sh`): each plants a defect in a throwaway copy of the tree, runs the
gate as a control first, then requires the gate to fail and name the culprit. Governed path (FACTORY.md).
Now: `neg_format.sh` (`just neg-format`).
Also: `neg_doc.sh` (`just neg-doc`), `neg_registry.sh` (`just neg-registry`), `doctor_selftest.sh` (`just doctor-selftest`).
Also: `neg_test_timeout.sh` (`just neg-test-timeout`), `neg_format_all.sh` (`just neg-format-all`).
Also: `sanitize_proof.sh` (`just sanitize-proof`), `neg_tidy.sh` (`just neg-tidy`), `neg_golden.sh`, `neg_provenance.sh`, `neg_log_schema.sh`, `neg_deps_license.sh`, `neg_ci_local_red.sh`, `neg_vcpkg_absent.sh`.
