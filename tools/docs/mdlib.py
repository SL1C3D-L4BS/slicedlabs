"""Markdown helpers shared by build.py and check.py (WO-031). Stdlib only, no subprocess, no network (ADR-0004)."""

import os
import posixpath
import re

FENCE_RE = re.compile(r"^\s{0,3}(`{3,}|~{3,})")
CODE_SPAN_RE = re.compile(r"(`+)(.+?)\1")
# [text](target "title") and <target> forms; the target has no spaces.
INLINE_LINK_RE = re.compile(
    r"(!?\[(?:[^\[\]]|\[[^\]]*\])*\])\(\s*<?([^()\s>]*(?:\([^()\s]*\)[^()\s>]*)*)>?(\s+\"[^\"]*\")?\s*\)"
)
REF_DEF_RE = re.compile(r"^(\s{0,3}\[[^\]]+\]:\s*)(\S+)(.*)$")
HEADING_RE = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
EXTERNAL_RE = re.compile(r"^(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//)")


def lines_outside_fences(text):
    """Yield (line_number, line) for every line that is not inside a fenced code block (fences excluded)."""
    fence = None
    for n, line in enumerate(text.split("\n"), 1):
        m = FENCE_RE.match(line)
        if fence is None:
            if m:
                fence = m.group(1)[0] * len(m.group(1))
                continue
            yield n, line
        elif m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence) and line.strip() == m.group(1):
            fence = None


def strip_code_spans(line):
    return CODE_SPAN_RE.sub(lambda m: " " * len(m.group(0)), line)


def slugify(heading):
    """GitHub-style anchor of a heading text."""
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    text = text.replace("`", "")
    text = text.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors(text):
    """Set of heading anchors of a document (duplicates get -1, -2 ...)."""
    seen = {}
    out = set()
    for _, line in lines_outside_fences(text):
        m = HEADING_RE.match(line)
        if not m:
            continue
        slug = slugify(m.group(2).replace("*", ""))
        count = seen.get(slug, 0)
        seen[slug] = count + 1
        out.add(slug if count == 0 else "%s-%d" % (slug, count))
    return out


def links(text):
    """Yield (line_number, target) for every inline or reference-style link outside fences and code spans."""
    for n, line in lines_outside_fences(text):
        bare = strip_code_spans(line)
        for m in INLINE_LINK_RE.finditer(bare):
            yield n, m.group(2)
        m = REF_DEF_RE.match(bare)
        if m:
            yield n, m.group(2)


def is_external(target):
    return bool(EXTERNAL_RE.match(target))


def rebase(text, src_dir, dst_dir):
    """Rewrite relative link targets of markdown living in src_dir so they work from dst_dir
    (repo-relative posix dirs)."""

    def fix(target):
        if not target or is_external(target) or target.startswith("#") or target.startswith("/"):
            return target
        path, sep, frag = target.partition("#")
        joined = posixpath.normpath(posixpath.join(src_dir, path))
        rel = posixpath.relpath(joined, dst_dir or ".")
        return rel + sep + frag

    out = []
    for _, line in _lines_marked(text):
        if line[0]:
            out.append(line[1])
            continue
        raw = line[1]
        spans = []
        masked = CODE_SPAN_RE.sub(lambda m: spans.append(m.group(0)) or "\x00%d\x00" % (len(spans) - 1), raw)
        masked = INLINE_LINK_RE.sub(lambda m: "%s(%s%s)" % (m.group(1), fix(m.group(2)), m.group(3) or ""), masked)
        m = REF_DEF_RE.match(masked)
        if m:
            masked = m.group(1) + fix(m.group(2)) + m.group(3)
        out.append(re.sub("\x00(\\d+)\x00", lambda k: spans[int(k.group(1))], masked))
    return "\n".join(out)


def _lines_marked(text):
    """Yield (n, (in_fence, line)) for every line."""
    fence = None
    for n, line in enumerate(text.split("\n"), 1):
        m = FENCE_RE.match(line)
        if fence is None:
            if m:
                fence = m.group(1)[0] * len(m.group(1))
                yield n, (True, line)
            else:
                yield n, (False, line)
        else:
            yield n, (True, line)
            if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence) and line.strip() == m.group(1):
                fence = None


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def first_heading(text, default=""):
    for _, line in lines_outside_fences(text):
        m = HEADING_RE.match(line)
        if m and len(m.group(1)) == 1:
            return m.group(2)
    return default


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
