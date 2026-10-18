# tests/fixtures/fabric/tree

Golden data of the revision-store trie (WO-046, SL-ARCH-001 ARCH1-R51, FAB-INV-REV-01).

| File | Holds |
| :---- | :---- |
| `roots.json` | root digests and depths of seeded entry sets; `tests/unit/fabric/tree_history_test.cpp` rebuilds each set and compares |

The roots were computed twice: by the C++ trie and by an independent Python model of the node layout in
`engine/fabric/include/sl/fabric/tree.hpp` over `tools/fabric/slb_ref.py`. A change to the node layout changes every
root: regenerate, review, then `python3 -I tools/testing/fixtures.py lock`.
