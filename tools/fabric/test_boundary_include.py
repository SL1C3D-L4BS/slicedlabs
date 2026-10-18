"""Tests for rule R-BND-INCLUDE of the surface lint (WO-061, SL-ARCH-001 ARCH1-R61, FAB-INV-BND-03).

engine/runtime and engine/render must not include sl/fabric/internal/** or the headers of the mutation capability
(store.hpp declares it, f5.hpp scopes it). Every case plants its files in a temporary tree under a tempfile directory
(never in the repository) and runs the lint over it with --root.
"""

import contextlib
import importlib.util
import io
import os
import tempfile
import unittest

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("surface_check", os.path.join(_here, "surface_check.py"))
sc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sc)

ROOT = os.path.dirname(os.path.dirname(_here))

CLEAN = """// reads a projection and proposes patches
#include <vector>
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"

void tick();
"""


def run(root):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = sc.main(["surface_check.py", "--root", root])
    return status, out.getvalue()


def plant(root, rel, text):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def findings(output):
    return [line for line in output.splitlines() if "R-BND-INCLUDE" in line]


class BoundaryIncludeTest(unittest.TestCase):
    def test_fab_inv_bnd_03_include(self):
        """A planted include of an internal header, from runtime and from render, fails naming file:line."""
        with tempfile.TemporaryDirectory() as tmp:
            plant(tmp, "engine/runtime/src/sim.cpp", CLEAN)
            plant(tmp, "engine/render/src/draw.cpp", CLEAN)
            status, out = run(tmp)
            self.assertEqual(status, 0, out)
            self.assertIn("fabric-surface-check: ok", out)
            self.assertIn("2 runtime/render file(s) scanned", out)

            plant(tmp, "engine/runtime/src/sim.cpp",
                  '#include <vector>\n\n#include "sl/fabric/internal/trie.hpp"\n\nvoid tick();\n')
            plant(tmp, "engine/render/src/draw.cpp",
                  CLEAN + "#include <sl/fabric/internal/arena.hpp>\n")
            status, out = run(tmp)
            self.assertEqual(status, 1, out)
            hits = findings(out)
            self.assertEqual(len(hits), 2, out)
            self.assertTrue(hits[0].startswith("engine/render/src/draw.cpp:8: R-BND-INCLUDE:"), hits[0])
            self.assertIn("sl/fabric/internal/arena.hpp", hits[0])
            self.assertTrue(hits[1].startswith("engine/runtime/src/sim.cpp:3: R-BND-INCLUDE:"), hits[1])
            self.assertIn("sl/fabric/internal/trie.hpp", hits[1])
            self.assertIn("fabric-surface-check: FAIL", out)

    def test_fab_inv_bnd_03_include_capability(self):
        """The capability declarations (store.hpp, f5.hpp) are forbidden too, however the path is spelled."""
        spellings = [
            '#include "sl/fabric/store.hpp"',
            "#include <sl/fabric/store.hpp>",
            "#  include  <sl/fabric/f5.hpp>",
            "\t#include\t\"sl/fabric/f5.hpp\"",
            '#include "../../fabric/include/sl/fabric/store.hpp"',
            '#include "../fabric/./internal/x.hpp"',
            '#include "fabric/internal/x.hpp"',
            "#import <sl/fabric/store.hpp>",
        ]
        for spelling in spellings:
            with self.subTest(spelling=spelling), tempfile.TemporaryDirectory() as tmp:
                plant(tmp, "engine/runtime/include/a.hpp", "#pragma once\n// header\n%s\n" % spelling)
                status, out = run(tmp)
                self.assertEqual(status, 1, out)
                self.assertEqual(len(findings(out)), 1, out)
                self.assertTrue(findings(out)[0].startswith("engine/runtime/include/a.hpp:3: R-BND-INCLUDE:"),
                                out)

    def test_fab_inv_bnd_03_include_not_flagged(self):
        """Public read-side headers, comments, strings, Fabric's own sources and non-C++ files pass."""
        with tempfile.TemporaryDirectory() as tmp:
            plant(tmp, "engine/runtime/src/ok.cpp", CLEAN + '#include "sl/fabric/memory_backend.hpp"\n')
            plant(tmp, "engine/runtime/src/comments.cpp",
                  '// #include "sl/fabric/internal/x.hpp"\n'
                  '/* #include <sl/fabric/store.hpp> */\n'
                  '/*\n#include "sl/fabric/f5.hpp"\n*/\n'
                  'const char *text = "#include <sl/fabric/internal/y.hpp>";\n'
                  '#include "sl/fabric/boundary.hpp"\n'
                  '#include "my/store.hpp"\n'
                  '#include "sl/fabric/store_backend_notes.hpp"\n')
            plant(tmp, "engine/runtime/README.md", '#include "sl/fabric/internal/x.hpp"\n')
            plant(tmp, "engine/render/notes.txt", '#include "sl/fabric/store.hpp"\n')
            # Fabric itself and other engines may include what they like: the rule is about the consumers.
            plant(tmp, "engine/fabric/src/tx.cpp", '#include "sl/fabric/internal/x.hpp"\n')
            plant(tmp, "engine/core/src/c.cpp", '#include "sl/fabric/store.hpp"\n')
            plant(tmp, "tests/unit/runtime/t.cpp", '#include "sl/fabric/internal/x.hpp"\n')
            status, out = run(tmp)
            self.assertEqual(status, 0, out)
            self.assertEqual(findings(out), [])
            self.assertIn("2 runtime/render file(s) scanned", out)

    def test_fab_inv_bnd_03_include_nested_and_every_extension(self):
        """Files in nested directories and with any C++ extension are scanned; every offence is reported."""
        with tempfile.TemporaryDirectory() as tmp:
            names = ["a.hpp", "b.h", "c.cc", "d.cxx", "e.inl", "f.cpp"]
            for i, name in enumerate(names):
                plant(tmp, "engine/render/deep/er/%s" % name, '#include "sl/fabric/internal/h%d.hpp"\n' % i)
            plant(tmp, "engine/runtime/two.cpp",
                  '#include "sl/fabric/store.hpp"\n#include "sl/fabric/internal/z.hpp"\n#include <sl/fabric/f5.hpp>\n')
            status, out = run(tmp)
            self.assertEqual(status, 1, out)
            hits = findings(out)
            self.assertEqual(len(hits), len(names) + 3, out)
            self.assertEqual([h.split(": ")[0] for h in hits[-3:]],
                             ["engine/runtime/two.cpp:%d" % n for n in (1, 2, 3)])

    def test_fab_inv_bnd_03_include_allow_list_cannot_excuse(self):
        """An allow-list entry naming R-BND-INCLUDE is not a rule of the allow-list (R-F5-ALLOW), so no excuse exists."""
        with tempfile.TemporaryDirectory() as tmp:
            plant(tmp, "engine/runtime/a.cpp", '#include "sl/fabric/internal/x.hpp"\n')
            allow = os.path.join(tmp, "allow.txt")
            with open(allow, "w", encoding="utf-8") as fh:
                fh.write("engine/runtime/a.cpp:R-BND-INCLUDE:sl/fabric/internal/x.hpp not allowed\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                status = sc.main(["surface_check.py", "--root", tmp, "--allow", allow])
            self.assertEqual(status, 1)
            self.assertIn("R-F5-ALLOW", out.getvalue())
            self.assertEqual(len(findings(out.getvalue())), 2, out.getvalue())  # the include and the bad entry

    def test_fab_inv_bnd_03_include_repository(self):
        """The repository passes: engine/runtime and engine/render include no Fabric internals."""
        status, out = run(ROOT)
        self.assertEqual(status, 0, out)
        self.assertEqual(findings(out), [])


if __name__ == "__main__":
    unittest.main()
