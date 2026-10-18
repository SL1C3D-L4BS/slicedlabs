"""tidy_files: print the run-clang-tidy file arguments (one anchored regex per line) for `just tidy`.

Usage: python3 -I tests/proofs/tidy_files.py <compile_commands.json> <tidy-allow.txt>

First-party sources are the compile-database entries under engine/, examples/ and tests/, minus tests/proofs
(planted defects) and minus the files listed in tidy-allow.txt (`path | reason`, relative to the repository root).
Skipped files are named on stderr with their reason. Exit 2 on a malformed allow-list or an empty source set.
"""

import json
import os
import re
import sys

FIRST_PARTY = ("engine", "examples", "tests")
EXCLUDED = ("tests/proofs/",)


def read_allow(path):
    allow = {}
    if not os.path.exists(path):
        return allow
    with open(path, encoding="utf-8") as fh:
        for number, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            name, sep, reason = (part.strip() for part in line.partition("|"))
            if not sep or not name or not reason:
                print("tidy: %s:%d: want `path | reason`, got %r" % (path, number, line), file=sys.stderr)
                sys.exit(2)
            allow[name] = reason
    return allow


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    db_path, allow_path = argv[1], argv[2]
    root = os.getcwd()
    with open(db_path, encoding="utf-8") as fh:
        entries = json.load(fh)
    allow = read_allow(allow_path)
    files = []
    skipped = {}
    for entry in entries:
        rel = os.path.relpath(os.path.join(entry.get("directory", root), entry["file"]), root)
        if rel.split(os.sep)[0] not in FIRST_PARTY or rel.startswith(EXCLUDED):
            continue
        if rel in allow:
            skipped[rel] = allow[rel]
        elif rel not in files:
            files.append(rel)
    for rel, reason in sorted(skipped.items()):
        print("tidy: skipped %s (tidy-allow.txt: %s)" % (rel, reason), file=sys.stderr)
    for rel in sorted(set(allow) - set(skipped)):
        print("tidy: note: tidy-allow.txt lists %s, which is not a first-party source of the build" % rel,
              file=sys.stderr)
    if not files:
        print("tidy: no first-party sources in %s" % db_path, file=sys.stderr)
        return 2
    for rel in sorted(files):
        print("^" + re.escape(os.path.join(root, rel)) + "$")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
