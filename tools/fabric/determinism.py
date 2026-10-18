"""fabric-determinism comparator (WO-041, FAB-INV-SER-04, SL-ARCH-001 ARCH1-R31). Stdlib only (ADR-0004).

    python3 -I tools/fabric/determinism.py <corpus-clang.json> <corpus-gcc.json> <accept.json>
    python3 -I tools/fabric/determinism.py --synthesize <accept.json> <out.json>

The first form compares the two outputs of `sl_fabric_corpus` (engine/fabric/tools/sl_fabric_corpus.cpp, format
sl-fabric-corpus/1: {"vectors": [{name, hex, oid, text?}...]}) built by clang and by gcc:

  R-DET-COMPILER  the two files are not byte-identical; names the first vector whose row differs
  R-DET-CORPUS    a row differs from accept.json (hex, oid, or text when present), or a vector is missing, extra or
                  out of order; names the first such vector
  R-DET-FORMAT    a file is not a corpus output (bad JSON, wrong format, a row without name, hex or oid)

and exits 1 naming the first failure, 0 with a summary otherwise. The second form writes the output the C++ emitter
should produce for an accept corpus (hex, oid and text copied); the neg test (tests/gates/neg_fabric_determinism.sh)
builds its planted copies from it.
"""

import json
import sys

FORMAT = "sl-fabric-corpus/1"


class DeterminismError(Exception):
    def __init__(self, rule, message):
        super().__init__("%s: %s" % (rule, message))
        self.rule = rule


def load_rows(path):
    try:
        with open(path, "rb") as handle:
            raw = handle.read()
        doc = json.loads(raw.decode("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        raise DeterminismError("R-DET-FORMAT", "%s: %s" % (path, exc)) from None
    if not isinstance(doc, dict) or doc.get("format") != FORMAT or not isinstance(doc.get("vectors"), list):
        raise DeterminismError("R-DET-FORMAT", "%s: expected format %s with a vectors list" % (path, FORMAT))
    for row in doc["vectors"]:
        if not isinstance(row, dict) or not all(isinstance(row.get(k), str) for k in ("name", "hex", "oid")):
            raise DeterminismError("R-DET-FORMAT", "%s: a row without name, hex or oid: %r" % (path, row))
    return raw, doc["vectors"]


def load_accept(path):
    try:
        with open(path, encoding="utf-8") as handle:
            doc = json.load(handle)
    except (OSError, ValueError) as exc:
        raise DeterminismError("R-DET-FORMAT", "%s: %s" % (path, exc)) from None
    vectors = doc.get("vectors") if isinstance(doc, dict) else None
    if not isinstance(vectors, list):
        raise DeterminismError("R-DET-FORMAT", "%s: expected a vectors list" % path)
    return vectors


def first_difference(left, right):
    """The name of the first row that differs between two row lists (or the first unmatched one)."""
    for a, b in zip(left, right):
        if a != b:
            return a.get("name") if a.get("name") == b.get("name") else "%s/%s" % (a.get("name"), b.get("name"))
    if len(left) != len(right):
        longer = left if len(left) > len(right) else right
        return longer[min(len(left), len(right))].get("name")
    return None


def compare(clang_path, gcc_path, accept_path):
    """Return a summary line; raise DeterminismError naming the first failure."""
    clang_raw, clang_rows = load_rows(clang_path)
    gcc_raw, gcc_rows = load_rows(gcc_path)
    if clang_raw != gcc_raw:
        name = first_difference(clang_rows, gcc_rows)
        if name is None:
            raise DeterminismError("R-DET-COMPILER", "the outputs differ in layout only (same rows)")
        raise DeterminismError("R-DET-COMPILER", "vector '%s' differs between clang and gcc" % name)
    accept = load_accept(accept_path)
    texts = 0
    for i, want in enumerate(accept):
        if i >= len(clang_rows):
            raise DeterminismError("R-DET-CORPUS", "vector '%s' is missing from the C++ output" % want["name"])
        got = clang_rows[i]
        if got["name"] != want["name"]:
            raise DeterminismError(
                "R-DET-CORPUS", "vector '%s' expected at row %d, got '%s'" % (want["name"], i, got["name"]))
        for key in ("hex", "oid"):
            if got[key] != want[key]:
                raise DeterminismError(
                    "R-DET-CORPUS", "vector '%s': %s %s != accept.json %s" % (want["name"], key, got[key], want[key]))
        if "text" in got:
            texts += 1
            if got["text"] != want["text"]:
                raise DeterminismError("R-DET-CORPUS", "vector '%s': text %r != accept.json %r"
                                       % (want["name"], got["text"], want["text"]))
    if len(clang_rows) > len(accept):
        raise DeterminismError("R-DET-CORPUS", "vector '%s' is not in accept.json" % clang_rows[len(accept)]["name"])
    return "%d vectors byte-identical on clang and gcc and equal to accept.json (hex, oid; %d texts)" % (
        len(accept), texts)


def synthesize(accept_path):
    rows = []
    for v in load_accept(accept_path):
        rows.append({"name": v["name"], "hex": v["hex"], "oid": v["oid"], "text": v["text"]})
    body = ",\n".join(json.dumps(r, ensure_ascii=False) for r in rows)
    return '{\n"format": "%s",\n"vectors": [\n%s\n]\n}\n' % (FORMAT, body)


def main(argv):
    if len(argv) == 4 and argv[1] == "--synthesize":
        try:
            text = synthesize(argv[2])
        except DeterminismError as exc:
            print("fabric-determinism: FAIL: %s" % exc)
            return 1
        with open(argv[3], "w", encoding="utf-8") as handle:
            handle.write(text)
        return 0
    if len(argv) != 4:
        print(__doc__.strip())
        return 2
    try:
        summary = compare(argv[1], argv[2], argv[3])
    except DeterminismError as exc:
        print("fabric-determinism: FAIL: %s" % exc)
        return 1
    print("fabric-determinism: ok (%s)" % summary)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
