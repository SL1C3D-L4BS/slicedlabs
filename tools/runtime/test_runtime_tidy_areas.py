"""Tests for the runtime-tidy area map (WO-109): every .cpp under engine/runtime/testing is named in an area of
scripts/runtime/runtime-tidy.sh, so `just runtime-tidy <area>` checks it, and every testing/ entry of the map exists.
The map is read as text; no subprocess.
"""

import os
import re
import unittest

_here = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(_here))
SCRIPT = os.path.join(REPO, "scripts", "runtime", "runtime-tidy.sh")
TESTING = os.path.join(REPO, "engine", "runtime", "testing")

_ENTRY = re.compile(r"^\s*\[([a-z]+)\]='([^']*)'\s*$")


def area_map(text):
    """The `declare -A sources=(...)` block of runtime-tidy.sh as {area: [patterns]}."""
    start = text.index("declare -A sources=(")
    end = text.index("\n)", start)
    areas = {}
    for line in text[start:end].splitlines()[1:]:
        m = _ENTRY.match(line)
        if m:
            areas[m.group(1)] = m.group(2).split()
    return areas


def testing_entries(areas):
    """{file name: [areas]} for the map's testing/ entries."""
    named = {}
    for area, patterns in areas.items():
        for p in patterns:
            if p.startswith("testing/"):
                named.setdefault(p[len("testing/"):], []).append(area)
    return named


class RuntimeTidyAreasTest(unittest.TestCase):
    def setUp(self):
        with open(SCRIPT, encoding="utf-8") as f:
            self.areas = area_map(f.read())

    def test_map_parses(self):
        self.assertIn("life", self.areas)
        self.assertIn("store", self.areas)

    def test_every_testing_source_is_mapped(self):
        named = testing_entries(self.areas)
        sources = sorted(n for n in os.listdir(TESTING) if n.endswith(".cpp"))
        self.assertTrue(sources)
        missing = [n for n in sources if n not in named]
        self.assertEqual(missing, [], "engine/runtime/testing files no runtime-tidy area names")

    def test_every_mapped_testing_file_exists(self):
        named = testing_entries(self.areas)
        stale = sorted(n for n in named if not os.path.isfile(os.path.join(TESTING, n)))
        self.assertEqual(stale, [], "runtime-tidy names testing files that do not exist")

    def test_a_dropped_entry_is_caught(self):
        text = "declare -A sources=(\n  [plat]='platform_*'\n)\n"
        named = testing_entries(area_map(text))
        self.assertNotIn("scripted_platform.cpp", named)


if __name__ == "__main__":
    unittest.main()
