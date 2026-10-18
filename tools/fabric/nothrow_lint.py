"""fabric-nothrow-check: the ADR-0009 / ADR-0016 nothrow lint for persisted-path code (FAB-INV-CF-04).

Scans engine/fabric/src, engine/fabric/include and engine/fabric/gen (never a `testing/` or `tools/` directory;
a directory that does not exist yet is skipped) and reports `path:line: RULE: message`:

  N-THROW      a `throw` expression
  N-TRY        a `try` or `catch` keyword
  N-AT         a `.at(` or `->at(` call (bounds-checked access throws)
  N-STO        std::stoi, stol, stoll, stoul, stoull, stof, stod, stold
  N-FS         a throwing std::filesystem call (or iterator construction) with no std::error_code argument
  N-UNORDERED  std::unordered_{map,set,multimap,multiset}: iteration order would leak into persisted bytes
  N-NEW        a naked `new` or `delete` expression (`= delete` and `operator new` declarations are not flagged)
  N-ALLOW      a malformed line in the allow-list (no reason, unknown rule, no `<path>:<rule>` head)

Comments and string or character literals are blanked before matching, so prose never trips a rule. The allow-list
(tools/fabric/nothrow-allow.txt) holds `<path>:<rule> <reason>` lines; `#` starts a comment; an entry without a
reason is itself an error. Stdlib only, no subprocess (ADR-0004); run as
`python3 -I tools/fabric/nothrow_lint.py [--root DIR] [--allow FILE] [--dirs a,b,c]`.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DIRS = ("engine/fabric/src", "engine/fabric/include", "engine/fabric/gen")
DEFAULT_ALLOW = os.path.join("tools", "fabric", "nothrow-allow.txt")
EXTENSIONS = (".cpp", ".hpp", ".h", ".cc", ".cxx", ".inl")
SKIP_DIRS = ("testing", "tools")
RULES = ("N-THROW", "N-TRY", "N-AT", "N-STO", "N-FS", "N-UNORDERED", "N-NEW")

FS_CALLS = (
    "absolute canonical weakly_canonical relative proximate copy copy_file copy_symlink create_directory "
    "create_directories create_hard_link create_symlink create_directory_symlink current_path equivalent exists "
    "file_size hard_link_count last_write_time permissions read_symlink remove remove_all rename resize_file space "
    "status symlink_status temp_directory_path is_block_file is_character_file is_directory is_empty is_fifo "
    "is_other is_regular_file is_socket is_symlink status_known"
).split()
_FS_NS = r"(?:std::filesystem|std::experimental::filesystem|fs)"
_FS_CALL = re.compile(r"\b" + _FS_NS + r"::(%s)\s*\(" % "|".join(FS_CALLS))
_FS_ITER = re.compile(r"\b" + _FS_NS + r"::(?:recursive_)?directory_iterator\b\s*(?:\w+\s*)?[({]")
_FS_ALIAS = re.compile(r"namespace\s+(\w+)\s*=\s*(?:std::filesystem|std::experimental::filesystem)\s*;")
_EC_DECL = re.compile(r"\b(?:std::)?error_code\s+(\w+)")

_PATTERNS = (
    ("N-THROW", re.compile(r"\bthrow\b"), "throw expression; return sl::result or sl::status instead (ADR-0009)"),
    ("N-TRY", re.compile(r"\b(?:try|catch)\b"), "try/catch; errors are values on the persisted path (ADR-0009)"),
    ("N-AT", re.compile(r"(?:\.|->)\s*at\s*\("), "at() throws on a bad index; check the bound and use operator[]"),
    ("N-STO", re.compile(r"\bstd::sto(?:i|l|ll|ul|ull|f|d|ld)\b"), "std::sto* throws; use std::from_chars"),
    ("N-UNORDERED", re.compile(r"\bstd::unordered_(?:multi)?(?:map|set)\b"),
     "unordered container: iteration order would leak into persisted bytes; use a sorted container"),
    ("N-NEW", re.compile(r"(?<!operator)(?<!operator )\bnew\b\s*[A-Za-z_:(\[]"),
     "naked new; use a value or smart pointer"),
    ("N-NEW", re.compile(r"(?<!=)(?<!= )(?<!operator)(?<!operator )\bdelete\b(?!\s*[;,)])"),
     "naked delete; use a value or smart pointer"),
)


def blank(text):
    """Return text with comments and string/char literal contents replaced by spaces; newlines are kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i : i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == "R" and text[i + 1 : i + 2] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            m = re.match(r'R"([^()\\ ]{0,16})\(', text[i:])
            end = text.find(")" + m.group(1) + '"', i) if m else -1
            if not m or end < 0:
                out.append(c)
                i += 1
                continue
            j = end + len(m.group(1)) + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == "'" and i > 0 and text[i - 1].isalnum():
            out.append(c)
            i += 1
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * max(j - i - 2, 0) + (c if j - i >= 2 else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _call_args(text, start):
    """Text between the bracket at `start - 1` and its match (paren or brace); empty when unbalanced."""
    opener = text[start - 1]
    closer = ")" if opener == "(" else "}"
    depth = 1
    for j in range(start, len(text)):
        if text[j] == opener:
            depth += 1
        elif text[j] == closer:
            depth -= 1
            if depth == 0:
                return text[start:j]
    return text[start:]


def _fs_findings(clean, lineno):
    """Yield (line, message) for std::filesystem calls without an error_code argument."""
    names = {m for m in _EC_DECL.findall(clean)}
    aliases = {m for m in _FS_ALIAS.findall(clean)}
    fs_call, fs_iter = _FS_CALL, _FS_ITER
    for alias in aliases - {"fs"}:
        fs_call = re.compile(fs_call.pattern.replace("|fs)", "|fs|%s)" % re.escape(alias)))
        fs_iter = re.compile(fs_iter.pattern.replace("|fs)", "|fs|%s)" % re.escape(alias)))
    for rx in (fs_call, fs_iter):
        for m in rx.finditer(clean):
            args = _call_args(clean, m.end())
            if rx is fs_iter and not args.strip():
                continue  # a default-constructed iterator is the end sentinel and cannot throw
            has_ec = "error_code" in args or any(re.search(r"\b%s\b" % re.escape(v), args) for v in names)
            if not has_ec:
                what = m.group(0).rstrip("({ ").split("::")[-1].split()[0]
                yield lineno(m.start()), "std::filesystem::%s without a std::error_code argument" % what


def lint_text(text):
    """Return a sorted list of (line, rule, message) for one source text."""
    clean = blank(text)
    starts = [0]
    for m in re.finditer("\n", clean):
        starts.append(m.end())

    def lineno(pos):
        lo, hi = 0, len(starts)
        while lo < hi - 1:
            mid = (lo + hi) // 2
            lo, hi = (mid, hi) if starts[mid] <= pos else (lo, mid)
        return lo + 1

    found = set()
    for rule, rx, message in _PATTERNS:
        for m in rx.finditer(clean):
            found.add((lineno(m.start()), rule, message))
    for line, message in _fs_findings(clean, lineno):
        found.add((line, "N-FS", message))
    return sorted(found)


def load_allow(path):
    """Return (set of (path, rule), error strings) from the allow-list file; a missing file is empty."""
    allowed, errors = set(), []
    try:
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return allowed, errors
    shown = os.path.basename(path)
    for n, raw in enumerate(lines, start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        head, _, reason = line.partition(" ")
        target, _, rule = head.rpartition(":")
        if not target or rule not in RULES:
            errors.append("%s:%d: N-ALLOW: expected `<path>:<rule> <reason>` with a rule in %s, got %r"
                          % (shown, n, ", ".join(RULES), line))
        elif not reason.strip():
            errors.append("%s:%d: N-ALLOW: entry %s has no reason" % (shown, n, head))
        else:
            allowed.add((target, rule))
    return allowed, errors


def source_files(root, dirs):
    """Yield relative paths of the sources under the scanned directories, skipping testing/ and tools/."""
    for rel_dir in dirs:
        top = os.path.join(root, *rel_dir.split("/"))
        for here, subdirs, files in os.walk(top):
            subdirs[:] = sorted(d for d in subdirs if d not in SKIP_DIRS)
            for name in sorted(files):
                if name.endswith(EXTENSIONS):
                    yield os.path.relpath(os.path.join(here, name), root).replace(os.sep, "/")


def lint_tree(root, dirs=DEFAULT_DIRS, allow_path=None):
    """Return (error lines, file count, allowed count)."""
    allowed, errors = load_allow(allow_path or os.path.join(root, DEFAULT_ALLOW))
    files = skipped = 0
    for rel in source_files(root, dirs):
        files += 1
        try:
            with open(os.path.join(root, *rel.split("/")), encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError as exc:
            errors.append("%s:1: N-ALLOW: unreadable: %s" % (rel, exc))
            continue
        for line, rule, message in lint_text(text):
            if (rel, rule) in allowed:
                skipped += 1
            else:
                errors.append("%s:%d: %s: %s" % (rel, line, rule, message))
    return errors, files, skipped


def main(argv):
    root, dirs, allow, args = ROOT, DEFAULT_DIRS, None, argv[1:]
    while args:
        if args[0] == "--root" and len(args) >= 2:
            root = os.path.abspath(args[1])
        elif args[0] == "--allow" and len(args) >= 2:
            allow = os.path.abspath(args[1])
        elif args[0] == "--dirs" and len(args) >= 2:
            dirs = tuple(d for d in args[1].split(",") if d)
        else:
            print("usage: nothrow_lint.py [--root DIR] [--allow FILE] [--dirs a,b,c]", file=sys.stderr)
            return 2
        args = args[2:]
    errors, files, skipped = lint_tree(root, dirs, allow)
    for e in errors:
        print(e)
    if errors:
        print("fabric-nothrow-check: FAIL (%d finding(s) in %d file(s))" % (len(errors), files))
        return 1
    note = "" if files else " (no sources under %s yet)" % ", ".join(dirs)
    print("fabric-nothrow-check: ok (%d file(s) scanned, %d allowed)%s" % (files, skipped, note))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
