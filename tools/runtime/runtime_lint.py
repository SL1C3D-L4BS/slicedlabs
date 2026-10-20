"""runtime-lint: the Volume 05 source guardrails (WO-077, SL-ARCH-003 ARCH3-R004 to R010, R035, R057, R058, R087).

Scans engine/runtime/** (C++ sources and headers, engine/runtime/tools CMake files) and scripts/runtime/*.sh and
reports `<path>:<line>: <RULE>: <message>`; the exit status is 1 on any finding. Comments, string literals and the
path of an #include line are blanked before the code rules match (the same approach as tools/fabric/surface_check.py,
with its own helpers).

  R-RT-LAYER     a header of include/sl/runtime that includes a header of a higher layer of the SL-ARCH-003 `layers`
                 table, or a same-layer include cycle (L9: runtime.hpp and bridge.hpp include neither the other)
  R-RT-FABRIC    an sl/fabric include outside the two scoped allow-lists: include/ and src/ take boundary, id, value,
                 digest, schema, type, patch, events and clock; testing/ and tools/ also workspace, transaction and
                 authority; vfs.hpp only in src/platform_files*.cpp; internal/**, store.hpp and f5.hpp nowhere
  R-RT-TXCALL    transaction_manager, transaction, workspace or tx_options named under include/ or src/
  R-RT-MATH      a struct, class or using named vec*, mat*, quat*, transform*, fixed*, simd*, aabb* or curve*
                 (ADR-0021), or an include of an intrinsics or SIMD header
  R-RT-GLOBAL    a mutable namespace-scope, class-static or function-static variable (const and constexpr pass;
                 thread_local never does) that tools/runtime/lint_allow.json does not allow
  R-RT-FENCE     atomic_thread_fence, atomic_signal_fence, volatile, asm, __asm__, SYS_futex, syscall
  R-RT-ORDER     a memory_order weaker than seq_cst with no `// pairs: <symbol>` on the same or the previous line
  R-RT-DIST      std::*_distribution, std::random_device, rand(), srand(), std::hash outside src/platform_*.cpp, a
                 range-for over a std::unordered_* container
  R-RT-PLATFORM  ::now(), std::thread, std::jthread, fopen, ::open, std::filesystem, std::ifstream, std::ofstream
                 outside src/platform_*.cpp
  R-RT-THROW     throw, try, catch, `.at(`, std::sto*
  R-RT-NOEXCEPT  a lambda without noexcept passed to job_desc, system_desc, add_task, a method invoker or an event
                 handler
  R-RT-LINK      engine/runtime/tools/** CMake naming sl_fabric_testing or sl::fabric_testing
  R-RT-OUTPUT    a scripts/runtime/*.sh redirection, -o, --output, --evidence-dir, mkdir, cp, mv, tee or touch target
                 outside build/
  R-RT-ALLOW     a malformed lint_allow.json (an entry without a reason is an error)

lint_allow.json is {"allow": [{"path", "rule", "symbol", "reason"}]}: a finding is excused by an entry with its path,
rule and symbol (symbol "*" excuses every symbol of that file and rule). Stdlib only, no subprocess (ADR-0004):
`python3 -I tools/runtime/runtime_lint.py [--root DIR] [--allow FILE]`.
"""

import bisect
import fnmatch
import json
import os
import posixpath
import re
import shlex
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUNTIME_DIR = "engine/runtime"
SCRIPTS_DIR = "scripts/runtime"
DEFAULT_ALLOW = "tools/runtime/lint_allow.json"
RULES = ("R-RT-LAYER", "R-RT-FABRIC", "R-RT-TXCALL", "R-RT-MATH", "R-RT-GLOBAL", "R-RT-FENCE", "R-RT-ORDER",
         "R-RT-DIST", "R-RT-PLATFORM", "R-RT-THROW", "R-RT-NOEXCEPT", "R-RT-LINK", "R-RT-OUTPUT")
CPP_EXT = (".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp", ".cpp", ".cc", ".cxx", ".cppm", ".ixx")

FABRIC_BASE = ("boundary.hpp", "id.hpp", "value.hpp", "digest.hpp", "schema.hpp", "type.hpp", "patch.hpp",
               "events.hpp", "clock.hpp")
FABRIC_TOOLING = ("workspace.hpp", "transaction.hpp", "authority.hpp")
FABRIC_NEVER = ("store.hpp", "f5.hpp")

_INCLUDE = re.compile(r'^\s*#\s*(?:include|import)\s*([<"])([^>"]+)[>"]')
_DIRECTIVE = re.compile(r"^\s*#")
_INCLUDE_LINE = re.compile(r"^\s*#\s*(?:include|import)\b")
_BANNED_TYPE = re.compile(r"^(?:vec|vector|mat|matrix|quat|quaternion|transform|fixed|simd|aabb|curve)(?:\d|_|$)")
# names the document itself requires (ARCH3-R078 time::fixed_stepper); every other match is a finding
_MATH_ALLOWED = frozenset(("fixed_stepper",))
_MATH_DECL = re.compile(r"\b(?:struct|class|using)\s+(?:alignas\s*\([^)]*\)\s*)?([A-Za-z_]\w*)")
_MATH_INCLUDE = re.compile(r"(?:^|/)(?:\w*mmintrin\.h|x86intrin\.h|arm_neon\.h|arm_sve\.h|experimental/simd|simd)$")
_ORDER = re.compile(r"\b(?:memory_order_(?:relaxed|consume|acquire|release|acq_rel)|"
                    r"std::memory_order::(?:relaxed|consume|acquire|release|acq_rel))\b")
_PAIRS = re.compile(r"//\s*pairs:\s*\S")
_FENCE = re.compile(r"\b(?:atomic_thread_fence|atomic_signal_fence|volatile|asm|__asm__|__asm|SYS_futex|syscall)\b")
_TXCALL = re.compile(r"\b(?:transaction_manager|transaction|workspace|tx_options)\b")
_THROW = re.compile(r"\b(?:throw|try|catch)\b|(?:\.|->)\s*at\s*\(|\bstd::sto(?:i|l|ul|ll|ull|f|d|ld)\b")
_DIST = (
    (re.compile(r"\bstd::\w+_distribution\b"), "a std distribution is implementation-defined"),
    (re.compile(r"\bstd::random_device\b"), "std::random_device is not reproducible"),
    (re.compile(r"(?<![\w.>])(?:std::|::)?rand\s*\("), "rand() is not reproducible"),
    (re.compile(r"(?<![\w.>])(?:std::|::)?srand\s*\("), "srand() is not reproducible"),
)
_HASH = re.compile(r"\bstd::hash\b")
_PLATFORM = (
    (re.compile(r"::now\s*\("), "a clock's ::now()"),
    (re.compile(r"\bstd::(?:jthread|thread)\b(?!\s*::)"), "a std thread"),
    (re.compile(r"\bfopen\s*\("), "fopen"),
    (re.compile(r"(?<![\w>])::open\s*\("), "::open"),
    (re.compile(r"\bstd::filesystem\b(?!\s*::\s*path\b)"), "std::filesystem"),
    (re.compile(r"\bstd::(?:basic_)?[io]?fstream\b"), "a std file stream"),
)
_NOEXCEPT_CONTEXT = re.compile(
    r"\b(?:job_desc|system_desc|add_task|add_system|add_method|submit|try_submit|on_event|subscribe|"
    r"\w*invoker\w*|\w*handler\w*)\b|\.\s*fn\s*=")
_UNORDERED = re.compile(r"\bstd::unordered_\w+")
_OPERATOR = re.compile(
    r"\boperator\s*(?:\(\s*\)|\[\s*\]|<=>|<<=|>>=|->\*|->|<<|>>|&&|\|\||\+\+|--|[-+*/%^&|~!=<>]=?|,)"
)


# --- comment and string blanking ----------------------------------------------------------------------------------


def blank(text):
    """Return text with comments and string/char literal contents replaced by spaces; newlines and offsets kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c, two = text[i], text[i : i + 2]
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
        elif c == "R" and text[i + 1 : i + 2] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")
                                                           or text[i - 1] in "uUL8"):
            m = re.match(r'R"([^()\\\s]{0,16})\(', text[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            end = text.find(")" + m.group(1) + '"', i + m.end())
            j = n if end < 0 else end + len(m.group(1)) + 2
            out.append('R"' + "".join(ch if ch == "\n" else " " for ch in text[i + 2 : j - 1]) + '"')
            i = j
        elif c == "'" and i > 0 and text[i - 1].isalnum():
            out.append(c)  # a digit separator such as 1'000
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


def _mask_lines(text, keep_define):
    """Blank preprocessor lines (and their backslash continuations); #define lines stay when keep_define."""
    lines, out, skipping = text.split("\n"), [], False
    for line in lines:
        directive = bool(_DIRECTIVE.match(line))
        if skipping or (directive and not (keep_define and re.match(r"^\s*#\s*define\b", line))):
            out.append(" " * len(line))
            skipping = line.rstrip().endswith("\\")
        else:
            out.append(line)
            skipping = False
    return "\n".join(out)


class Source:
    """One C++ file: raw text, the blanked views and a line index."""

    def __init__(self, rel, text):
        self.rel = rel
        self.text = text
        self.raw_lines = text.split("\n")
        self.cleaned = blank(text)
        mask = [" " * len(c) if _INCLUDE_LINE.match(c) else c for c in self.cleaned.split("\n")]
        self.code = "\n".join(mask)  # include lines masked: an include path is not a use of a name
        self.scan = _mask_lines(self.code, keep_define=False)
        self.starts = [0] + [m.end() for m in re.finditer("\n", text)]
        parts = rel.split("/")
        self.zone = parts[2] if len(parts) > 3 and parts[:2] == ["engine", "runtime"] else ""
        self.base = parts[-1]
        self.in_src = self.zone == "src" and len(parts) == 4  # directly in engine/runtime/src
        self.is_platform = self.in_src and fnmatch.fnmatch(self.base, "platform_*.cpp")

    def line(self, pos):
        return bisect.bisect_right(self.starts, pos)

    def includes(self):
        """Yield (line, path, quoted) of every real include directive."""
        for n, (raw, clean) in enumerate(zip(self.raw_lines, self.cleaned.split("\n")), start=1):
            if not _INCLUDE_LINE.match(clean):
                continue
            m = _INCLUDE.match(raw)
            if m:
                yield n, m.group(2).strip(), m.group(1) == '"'


# --- the layers table -----------------------------------------------------------------------------------------------


def load_layers(root):
    """Return (rank: header path -> layer index, names: [layer name], flat: {layer index}, errors).

    flat holds the layers whose headers may not include each other."""
    arch = os.path.join(root, "docs", "arch")
    names = sorted(n for n in os.listdir(arch) if n.startswith("SL-ARCH-003-") and n.endswith(".md")) \
        if os.path.isdir(arch) else []
    if not names:
        missing = "docs/arch/SL-ARCH-003-*.md:1: R-RT-LAYER: the document with the `layers` table is missing"
        return {}, [], set(), [missing]
    rel = "docs/arch/" + names[0]
    try:
        with open(os.path.join(arch, names[0]), encoding="utf-8") as fh:
            text = fh.read()
    except (OSError, UnicodeDecodeError) as exc:
        return {}, [], set(), ["%s:1: R-RT-LAYER: unreadable: %s" % (rel, exc)]
    rows = None
    for block in re.findall(r"^```sl-data\n(.*?)\n```", text, re.S | re.M):
        try:
            data = json.loads(block)
        except ValueError:
            continue
        if isinstance(data, dict) and isinstance(data.get("layers"), list):
            rows = data["layers"]
            break
    if not rows:
        return {}, [], set(), ["%s:1: R-RT-LAYER: no sl-data block with a `layers` table" % rel]
    rank, layer_names, flat = {}, [], set()
    for i, row in enumerate(rows):
        layer_names.append(str(row.get("layer", "L%d" % i)))
        headers = str(row.get("headers", ""))
        if "neither includes the other" in headers:
            flat.add(i)
        for part in re.sub(r"\([^)]*\)", "", headers).split(","):
            if part.strip():
                rank[part.strip()] = i
    return rank, layer_names, flat, []


def _sccs(graph):
    """Tarjan: the strongly connected components of graph {node: set(node)}."""
    index, low, on, stack, out, counter = {}, {}, set(), [], [], [0]

    def visit(v):
        index[v] = low[v] = counter[0]
        counter[0] += 1
        stack.append(v)
        on.add(v)
        for w in sorted(graph.get(v, ())):
            if w not in index:
                visit(w)
                low[v] = min(low[v], low[w])
            elif w in on:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = set()
            while True:
                w = stack.pop()
                on.discard(w)
                comp.add(w)
                if w == v:
                    break
            out.append(comp)

    for v in sorted(graph):
        if v not in index:
            visit(v)
    return out


def layer_findings(sources, layers):
    """R-RT-LAYER over the headers of include/sl/runtime."""
    rank, names, flat, _errors = layers
    out = []
    prefix = "engine/runtime/include/sl/runtime/"
    graph, edges = {}, {}
    for src in sources:
        if not src.rel.startswith(prefix):
            continue
        own = src.rel[len(prefix):]
        if own not in rank:
            continue
        for line, path, quoted in src.includes():
            if path.startswith("sl/runtime/"):
                target = path[len("sl/runtime/"):]
            elif quoted:
                target = posixpath.normpath(posixpath.join(posixpath.dirname(own), path))
            else:
                continue
            if target not in rank:
                continue
            if rank[target] > rank[own]:
                out.append((src.rel, line, "R-RT-LAYER", target,
                            "%s (%s) includes %s (%s), a higher layer: a header includes only its own or a lower layer"
                            % (own, names[rank[own]], target, names[rank[target]])))
            elif rank[target] == rank[own]:
                graph.setdefault(own, set()).add(target)
                graph.setdefault(target, set())
                edges.setdefault((own, target), (src.rel, line))
    for (own, target), (rel, line) in sorted(edges.items()):
        if rank[own] in flat:
            out.append((rel, line, "R-RT-LAYER", target,
                        "%s includes %s of the same layer %s, whose headers include neither the other"
                        % (own, target, names[rank[own]])))
    for comp in _sccs(graph):
        if len(comp) > 1 or any(v in graph[v] for v in comp):
            for (own, target), (rel, line) in sorted(edges.items()):
                if own in comp and target in comp and rank[own] not in flat:
                    cycle = ", ".join(sorted(comp))
                    out.append((rel, line, "R-RT-LAYER", target,
                                "%s includes %s: same-layer include cycle (%s)" % (own, target, cycle)))
    return out


# --- the per-file code rules ----------------------------------------------------------------------------------------


def _hits(src, rx, text=None):
    for m in rx.finditer(src.code if text is None else text):
        yield src.line(m.start()), m


def fabric_findings(src):
    out = []
    for line, path, _quoted in src.includes():
        m = re.search(r"(?:^|/)sl/fabric/(.+)$", path)
        if not m:
            continue
        sub = m.group(1)
        if sub.startswith("internal/") or "/internal/" in sub or sub in FABRIC_NEVER:
            why = "nothing under engine/runtime may include it (ARCH3-R005)"
        elif sub == "vfs.hpp":
            if src.in_src and fnmatch.fnmatch(src.base, "platform_files*.cpp"):
                continue
            why = "vfs.hpp is allowed only in src/platform_files*.cpp (ARCH3-R004)"
        elif sub in FABRIC_BASE or (src.zone in ("testing", "tools") and sub in FABRIC_TOOLING):
            continue
        elif sub in FABRIC_TOOLING:
            why = "it is allowed only under testing/ and tools/ (ARCH3-R005)"
        else:
            why = "it is not on the allow-list of ARCH3-R004/R005"
        out.append((src.rel, line, "R-RT-FABRIC", sub, "%s includes sl/fabric/%s, but %s" % (src.rel, sub, why)))
    return out


def txcall_findings(src):
    if src.zone not in ("include", "src"):
        return []
    return [(src.rel, line, "R-RT-TXCALL", m.group(0),
             "%s is named under %s/: write-back leaves only through link::proposal_sink (ARCH3-R006)"
             % (m.group(0), src.zone)) for line, m in _hits(src, _TXCALL)]


def math_findings(src):
    out = []
    for line, m in _hits(src, _MATH_DECL):
        name = m.group(1)
        if _BANNED_TYPE.match(name) and name not in _MATH_ALLOWED:
            out.append((src.rel, line, "R-RT-MATH", name,
                        "type %s looks like a mathematics type: ADR-0021 allows only the standard library (ARCH3-R007)"
                        % name))
    for line, path, _quoted in src.includes():
        if _MATH_INCLUDE.search(path):
            out.append((src.rel, line, "R-RT-MATH", path,
                        "includes the intrinsics or SIMD header %s (ADR-0021, ARCH3-R007)" % path))
    return out


def fence_findings(src):
    return [(src.rel, line, "R-RT-FENCE", m.group(0),
             "%s: synchronization uses std::atomic, mutexes and the std primitives only (ARCH3-R057)" % m.group(0))
            for line, m in _hits(src, _FENCE)]


def order_findings(src):
    out = []
    for line, m in _hits(src, _ORDER):
        here = src.raw_lines[line - 1] if line <= len(src.raw_lines) else ""
        prev = src.raw_lines[line - 2] if line >= 2 else ""
        if not (_PAIRS.search(here) or _PAIRS.search(prev)):
            out.append((src.rel, line, "R-RT-ORDER", m.group(0),
                        "%s without a `// pairs: <symbol>` comment on this or the previous line (ARCH3-R058)"
                        % m.group(0)))
    return out


def dist_findings(src, unordered_names):
    out = []
    for rx, why in _DIST:
        out.extend((src.rel, line, "R-RT-DIST", m.group(0).strip("( "),
                    "%s: %s (ARCH3-R087)" % (m.group(0).strip("( "), why)) for line, m in _hits(src, rx))
    if not src.is_platform:
        out.extend((src.rel, line, "R-RT-DIST", "std::hash",
                    "std::hash outside src/platform_*.cpp: its values are implementation-defined (ARCH3-R087)")
                   for line, _m in _hits(src, _HASH))
    code = src.code
    for m in re.finditer(r"\bfor\s*\(", code):
        open_pos = m.end() - 1
        close = _match(code, open_pos, "(", ")")
        header = code[open_pos + 1 : close]
        colon = _range_colon(header)
        if colon < 0:
            continue
        expr = header[colon + 1 :]
        names = set(re.findall(r"[A-Za-z_]\w*", expr))
        hit = _UNORDERED.search(expr) or sorted(names & unordered_names)
        if hit:
            what = hit.group(0) if hasattr(hit, "group") else hit[0]
            out.append((src.rel, src.line(open_pos), "R-RT-DIST", what,
                        "range-for over the unordered container %s: its order is unspecified (ARCH3-R087)" % what))
    return out


def platform_findings(src):
    if src.is_platform:
        return []
    out = []
    for rx, what in _PLATFORM:
        out.extend((src.rel, line, "R-RT-PLATFORM", m.group(0).strip("( "),
                    "%s outside src/platform_*.cpp: reach the OS only through the platform interfaces (ARCH3-R035)"
                    % what) for line, m in _hits(src, rx))
    return out


def throw_findings(src):
    return [(src.rel, line, "R-RT-THROW", m.group(0).strip(),
             "%s: engine/runtime reports errors as sl::result values (ARCH3-R010)" % m.group(0).strip())
            for line, m in _hits(src, _THROW)]


def _match(text, open_pos, op, cl):
    depth = 0
    for j in range(open_pos, len(text)):
        if text[j] == op:
            depth += 1
        elif text[j] == cl:
            depth -= 1
            if depth == 0:
                return j
    return len(text)


def _range_colon(header):
    """Index of the `:` of a range-for header, or -1 for a classic for loop."""
    depth = 0
    colon = -1
    for j, ch in enumerate(header):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif depth == 0:
            if ch == ";":
                return -1
            if ch == ":" and header[j - 1 : j] != ":" and header[j + 1 : j + 2] != ":" and colon < 0:
                colon = j
    return colon


def _call_context(code, i):
    """The text of the statement before offset i: back to the previous `;` or to the `{` of an enclosing block."""
    depth, j = 0, i - 1
    while j >= 0 and i - j < 2000:
        ch = code[j]
        if ch in ")]}":
            depth += 1
        elif ch in "([":
            depth = max(depth - 1, 0)
        elif ch == "{":
            if depth:
                depth -= 1
            else:
                prev = code[:j].rstrip()
                word = re.search(r"(\w+)$", prev)
                if not prev or prev[-1] in ");{}" or (word and word.group(1) in (
                        "const", "noexcept", "override", "final", "else", "try", "do", "mutable")):
                    break  # the brace opens a block, not a braced initializer such as job_desc{
        elif ch == ";" and depth == 0:
            break
        j -= 1
    return code[j + 1 : i]


def noexcept_findings(src):
    out = []
    code = src.code
    n = len(code)
    for m in re.finditer(r"\[", code):
        i = m.start()
        if code[i + 1 : i + 2] == "[" or code[i - 1 : i] == "[":
            continue  # an attribute such as [[nodiscard]]
        k = i - 1
        while k >= 0 and code[k] in " \t\n":
            k -= 1
        if k >= 0 and (code[k].isalnum() or code[k] == "_"):
            word = re.search(r"(\w+)$", code[: k + 1])
            if not (word and word.group(1) == "return"):
                continue  # a subscript, a structured binding or a new[]
        elif k >= 0 and code[k] in ")]>":
            continue
        close = _match(code, i, "[", "]")
        j = close + 1
        while j < n and code[j] in " \t\n":
            j += 1
        if code[j : j + 1] == "<":
            j = _match(code, j, "<", ">") + 1
            while j < n and code[j] in " \t\n":
                j += 1
        if code[j : j + 1] == "(":
            j = _match(code, j, "(", ")") + 1
        while j < n and code[j] not in "{;,)=":
            if code[j] == "(":
                j = _match(code, j, "(", ")")
            j += 1
        if j >= n or code[j] != "{":
            continue
        if re.search(r"\bnoexcept\b", code[close + 1 : j]):
            continue
        context = _NOEXCEPT_CONTEXT.search(_call_context(code, i))
        if context:
            out.append((src.rel, src.line(i), "R-RT-NOEXCEPT", "lambda",
                        "a lambda passed to %s is not noexcept: job and system bodies, method invokers and event "
                        "handlers are noexcept by type (ARCH3-R010)" % (context.group(0).strip(". =") or "fn")))
    return out


# --- R-RT-GLOBAL ----------------------------------------------------------------------------------------------------


def _strip_balanced(text, op, cl):
    out, depth = [], 0
    for ch in text:
        if ch == op:
            depth += 1
            if depth == 1:
                out.append(" ")
        elif ch == cl and depth:
            depth -= 1
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def _classify_brace(stmt):
    """Kind of the block a `{` opens at namespace or class scope: ns, class, enum, func or init."""
    s = " ".join(stmt.split())
    if s.startswith("template"):
        s = re.sub(r"^template\s*", "", _strip_balanced(s, "<", ">"))
    s = re.sub(r"\[\[[^\]]*\]\]", " ", s)
    s = re.sub(r"\balignas\s*\([^)]*\)", " ", s).strip()
    if not s:
        return "func"  # a body after a constructor initializer list, or a bare block
    if re.search(r"\bnamespace\b", s) and "(" not in s or re.match(r"^extern\b[^()]*$", s):
        return "ns"
    if re.match(r"^enum\b", s):
        return "enum"
    if re.match(r"^(?:typedef\s+)?(?:class|struct|union)\b", s) and "(" not in s and "=" not in s:
        return "class"
    if "(" not in s:
        return "init"
    return "func"


def _is_const(head):
    head = _strip_balanced(head, "<", ">")
    head = re.sub(r"\[[^\]]*\]", " ", head)
    if re.search(r"\b(?:constexpr|consteval)\b", head):
        return True
    if "*" in head:
        head = head.rsplit("*", 1)[1]
    return bool(re.search(r"\bconst\b", head))


def _global_statement(src, text, start, scope):
    """Return (line, symbol, message) when one statement declares mutable static state, else None."""
    t = " ".join(text.split())
    t = _OPERATOR.sub("operator", t)
    while True:
        m = re.match(r"^(?:public|private|protected)\s*:\s*", t)
        if not m:
            break
        t = t[m.end():]
    if t.startswith("template"):
        t = _strip_balanced(t, "<", ">")
        t = re.sub(r"^template\s*", "", t)
    t = re.sub(r"\[\[[^\]]*\]\]", " ", t)
    t = re.sub(r"\balignas\s*\([^)]*\)", " ", t).strip()
    if not t or re.match(r"^(?:using|typedef|namespace|static_assert|friend|extern|concept|class|struct|union|enum|"
                         r"case|default|goto|break|continue|return|throw|delete|co_return)\b", t):
        return None
    is_static = bool(re.search(r"\bstatic\b", t))
    is_tls = bool(re.search(r"\bthread_local\b", t))
    if scope == "class" and not (is_static or is_tls):
        return None
    if scope == "code" and not re.match(r"^(?:(?:inline|constexpr|const|constinit|static|thread_local)\s+)*"
                                        r"(?:static|thread_local)\b", t):
        return None
    flat = _strip_balanced(t, "<", ">")
    paren = flat.find("(")
    eq = -1
    for m in re.finditer(r"=", flat):
        k = m.start()
        if flat[k - 1 : k] in ("=", "!", "<", ">") or flat[k + 1 : k + 2] == "=" or "operator" in flat[:k]:
            continue
        eq = k
        break
    if paren >= 0 and (eq < 0 or paren < eq):
        return None  # a function declaration or definition
    cut = len(flat)
    for ch in ("=", "{"):
        k = flat.find(ch)
        if 0 <= k < cut and (ch != "=" or k == eq):
            cut = k
    head = flat[:cut].strip()
    if not is_tls and _is_const(head):
        return None
    clean_head = re.sub(r"\[[^\]]*\]", " ", head).strip()
    m = re.search(r"([A-Za-z_]\w*)\s*$", clean_head)
    name = m.group(1) if m else "?"
    cut_orig = len(text)
    for ch in ("=", "{"):
        k = text.find(ch)
        if 0 <= k < cut_orig:
            cut_orig = k
    hits = [m.start() for m in re.finditer(r"\b%s\b" % re.escape(name), text[:cut_orig])] if name != "?" else []
    offset = start + (hits[-1] if hits else len(text) - len(text.lstrip()))
    what = "thread_local" if is_tls else ("static" if scope != "ns" else "namespace-scope")
    return (src.line(offset), name,
            "%s variable %s is mutable %s state: use const or constexpr, or pass the state through an object "
            "(ARCH3-R008)" % (what, name, "thread-local" if is_tls else "static"))


def global_findings(src):
    scan = src.scan
    n = len(scan)
    out = []
    stack = []  # (kind, saved paren depth)
    paren = 0
    stmt_start = 0
    i = 0

    def top():
        return stack[-1][0] if stack else "ns"

    while i < n:
        c = scan[i]
        if c == "(":
            paren += 1
        elif c == ")":
            paren = max(paren - 1, 0)
        elif c == ";" and paren == 0:
            if top() in ("ns", "class", "code"):
                found = _global_statement(src, scan[stmt_start:i], stmt_start, top())
                if found:
                    out.append((src.rel, found[0], "R-RT-GLOBAL", found[1], found[2]))
                stmt_start = i + 1
        elif c == "{":
            parent = top()
            before = scan[stmt_start:i]
            if parent in ("init", "enum", "inner"):
                kind = "inner"  # inside an initializer: nothing in it declares anything
            elif parent == "code":
                braced_init = before.rstrip().endswith(("=", ",")) or (
                    re.search(r"\b(?:static|thread_local)\b", before) and "(" not in before)
                kind = "init" if braced_init else "code"
            else:
                kind = "init" if paren > 0 else _classify_brace(before)
                kind = "code" if kind == "func" else kind
            if kind in ("ns", "class", "code"):
                stmt_start = i + 1
            stack.append((kind, paren))
            paren = 0
        elif c == "}":
            if stack:
                kind, paren = stack.pop()
                if kind in ("ns", "class", "code"):
                    stmt_start = i + 1
        i += 1
    return out


# --- the project-wide pieces ----------------------------------------------------------------------------------------


def unordered_names(sources):
    """Names declared (variables, members, parameters, functions) with a std::unordered_* type in the tree."""
    aliases = set()
    for src in sources:
        for m in re.finditer(r"\busing\s+(\w+)\s*=\s*std::unordered_\w+", src.code):
            aliases.add(m.group(1))
    names = set()
    for src in sources:
        code = src.code
        for m in re.finditer(r"\b(?:std::unordered_\w+|%s)\b" % "|".join(sorted(aliases) or ["(?!)"]), code):
            j = m.end()
            while j < len(code) and code[j] in " \t\n":
                j += 1
            if code[j : j + 1] == "<":
                j = _match(code, j, "<", ">") + 1
            d = re.match(r"[\s&*]*(?:const\s+)?[\s&*]*([A-Za-z_]\w*)\s*(?=[;={(\[,)])", code[j:])
            if d and d.group(1) not in ("const", "operator"):
                names.add(d.group(1))
    return names


def cmake_findings(root, rel_dir):
    """R-RT-LINK over every CMake file of engine/runtime/tools."""
    out = []
    top = os.path.join(root, *rel_dir.split("/"))
    for here, dirs, files in os.walk(top):
        dirs.sort()
        for name in sorted(files):
            if name != "CMakeLists.txt" and not name.endswith(".cmake"):
                continue
            full = os.path.join(here, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            try:
                with open(full, encoding="utf-8", errors="replace") as fh:
                    lines = fh.read().split("\n")
            except OSError as exc:
                out.append((rel, 1, "R-RT-LINK", "?", "unreadable: %s" % exc))
                continue
            for n, line in enumerate(lines, start=1):
                code = re.sub(r"#.*$", "", line)
                m = re.search(r"\bsl_fabric_testing\b|\bsl::fabric_testing\b", code)
                if m:
                    out.append((rel, n, "R-RT-LINK", m.group(0),
                                "%s names %s: the tools and the demo never link the fabric testing library "
                                "(ARCH3-R001)" % (rel, m.group(0))))
    return out


# --- R-RT-OUTPUT ----------------------------------------------------------------------------------------------------

_ASSIGN = re.compile(r"^\s*(?:(?:readonly|local|declare|export)\s+(?:-\w+\s+)?)?([A-Za-z_]\w*)=(.*)$")
_BUILD_SEG = re.compile(r"(?:^|/)build(?:/|$|[}\"'])")
_NO_WRITE = ("/dev/null", "/dev/stdout", "/dev/stderr", "/dev/tty")
_SEPARATORS = {";", ";;", "&&", "||", "|", "&", "(", ")", "{", "}", "then", "do", "else", "elif", "if", "while",
               "until", "!", "time", "|&"}
_FILE_REDIRECTS = {">", ">>", ">|", "&>", "&>>"}
_OTHER_REDIRECTS = {">&", "<&", "<", "<<", "<<-", "<<<", "<>"}
_OUT_OPTIONS = ("--output", "--out", "--evidence-dir", "--output-dir")
_NO_O_COMMANDS = {"grep", "egrep", "fgrep", "rg", "ls", "stat", "ps", "sed", "awk", "find", "git", "sort"}


def _build_rooted(value, rooted):
    v = value.strip().strip("\"'")
    if _BUILD_SEG.search(v) or v == "build" or v.startswith("./build"):
        return True
    m = re.match(r"^\$\{?(\w+)", v)
    return bool(m and m.group(1) in rooted)


def _target_ok(target, rooted):
    t = target.strip().strip("\"'")
    if t in _NO_WRITE or t.startswith("&") or t.startswith("/dev/fd/"):
        return True
    if not t:
        return False
    return _build_rooted(t, rooted)


def _tokens(line):
    lex = shlex.shlex(line, posix=True, punctuation_chars=True)
    lex.whitespace_split = True
    lex.commenters = "#"
    return list(lex)


def output_findings(rel, text):
    out = []
    rooted = set()
    lines = text.split("\n")
    heredoc = None
    joined = []
    i = 0
    while i < len(lines):
        start, buf = i + 1, lines[i]
        while buf.rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            buf = buf.rstrip()[:-1] + " " + lines[i]
        joined.append((start, buf))
        i += 1
    for n, line in joined:
        if heredoc is not None:
            if line.strip() == heredoc:
                heredoc = None
            continue
        m = _ASSIGN.match(line)
        if m and not line.lstrip().startswith("#"):
            if _build_rooted(m.group(2).split("#", 1)[0], rooted):
                rooted.add(m.group(1))
            else:
                rooted.discard(m.group(1))
        hd = re.search(r"<<-?\s*[\"']?(\w+)[\"']?", line)
        out.extend(_line_findings(rel, n, line, rooted))
        if hd:
            heredoc = hd.group(1)
    return out


def _line_findings(rel, n, line, rooted):
    """R-RT-OUTPUT findings of one logical shell line: its redirections and the commands that name a target."""
    line = re.sub(r"(?<![\w$-])\d(?=>)", " ", line)  # 2>file is a redirection of fd 2, not an argument
    try:
        tokens = _tokens(line)
    except ValueError:
        tokens = line.split()
    out, cmd, args, k = [], None, [], 0
    while k < len(tokens):
        tok = tokens[k]
        if tok in _SEPARATORS:
            if cmd:
                out.extend(_command_findings(rel, n, cmd, args, rooted))
            cmd, args = None, []
        elif tok in _FILE_REDIRECTS or (
            tok == ">&" and k + 1 < len(tokens) and not re.match(r"^(\d+|-)$", tokens[k + 1])
        ):
            target = tokens[k + 1] if k + 1 < len(tokens) else ""
            if cmd not in ("[", "[[", "test") and not _target_ok(target, rooted):
                out.append((rel, n, "R-RT-OUTPUT", target,
                            "redirection %s %s writes outside build/ (ARCH3-R015)" % (tok, target)))
            k += 1
        elif tok in _OTHER_REDIRECTS:
            k += 1  # a descriptor duplication or an input: no file is written
        elif cmd is None:
            if not (re.match(r"^[A-Za-z_]\w*=", tok) or tok in ("exec", "command", "sudo", "env", "nohup")):
                cmd = posixpath.basename(tok)
        else:
            args.append(tok)
        k += 1
    if cmd:
        out.extend(_command_findings(rel, n, cmd, args, rooted))
    return out


def _command_findings(rel, n, cmd, args, rooted):
    out = []

    def bad(target, what):
        if not _target_ok(target, rooted):
            out.append((rel, n, "R-RT-OUTPUT", target, "%s writes to %s, outside build/ (ARCH3-R015)" % (what, target)))

    for j, a in enumerate(args):
        nxt = args[j + 1] if j + 1 < len(args) else ""
        if a == "-o" and cmd not in _NO_O_COMMANDS and nxt:
            bad(nxt, "%s -o" % cmd)
        for opt in _OUT_OPTIONS:
            if a == opt and nxt:
                bad(nxt, "%s %s" % (cmd, opt))
            elif a.startswith(opt + "="):
                bad(a[len(opt) + 1 :], "%s %s" % (cmd, opt))
    operands = [a for a in args if not a.startswith("-")]
    if cmd in ("mkdir", "tee", "touch"):
        for a in operands:
            bad(a, cmd)
    elif cmd in ("cp", "mv", "install"):
        target = ""
        for j, a in enumerate(args):
            if a in ("-t", "--target-directory") and j + 1 < len(args):
                target = args[j + 1]
            elif a.startswith("--target-directory="):
                target = a.split("=", 1)[1]
        target = target or (operands[-1] if len(operands) >= 2 else "")
        if target:
            bad(target, cmd)
    return out


# --- the tree -------------------------------------------------------------------------------------------------------


def load_allow(path):
    """Return (list of entries, error strings); a missing file is an empty list."""
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except OSError:
        return [], []
    except ValueError as exc:
        return [], ["%s:1: R-RT-ALLOW: not JSON: %s" % (os.path.basename(path), exc)]
    shown = os.path.basename(path)
    entries = data.get("allow") if isinstance(data, dict) else None
    if not isinstance(entries, list):
        return [], ["%s:1: R-RT-ALLOW: expected {\"allow\": [...]}" % shown]
    good, errors = [], []
    for i, e in enumerate(entries):
        if not isinstance(e, dict) or set(e) != {"path", "rule", "symbol", "reason"}:
            errors.append("%s:1: R-RT-ALLOW: entry %d must be an object with exactly path, rule, symbol, reason"
                          % (shown, i))
        elif not all(isinstance(e[k], str) and e[k].strip() for k in e):
            has_reason = isinstance(e["reason"], str) and e["reason"].strip()
            reason = "has an empty field" if has_reason else "has no reason"
            errors.append("%s:1: R-RT-ALLOW: entry %d (%s %s) %s" % (shown, i, e.get("path"), e.get("rule"), reason))
        elif e["rule"] not in RULES:
            errors.append("%s:1: R-RT-ALLOW: entry %d names the unknown rule %s" % (shown, i, e["rule"]))
        else:
            good.append(e)
    return good, errors


def _walk(root, rel_dir, exts):
    top = os.path.join(root, *rel_dir.split("/"))
    for here, dirs, files in os.walk(top):
        dirs.sort()
        for name in sorted(files):
            if name.endswith(exts):
                full = os.path.join(here, name)
                yield os.path.relpath(full, root).replace(os.sep, "/"), full


def _read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return None


def check_tree(root, allow_path=None):
    """Return (error lines, C++ file count, script count)."""
    entries, errors = load_allow(allow_path or os.path.join(root, *DEFAULT_ALLOW.split("/")))
    sources, found = [], []
    for rel, full in _walk(root, RUNTIME_DIR, CPP_EXT):
        text = _read(full)
        if text is None:
            found.append((rel, 1, "R-RT-LAYER", "?", "unreadable"))
            continue
        sources.append(Source(rel, text))
    layers = load_layers(root)
    errors.extend(layers[3])
    names = unordered_names(sources)
    for src in sources:
        found.extend(fabric_findings(src))
        found.extend(txcall_findings(src))
        found.extend(math_findings(src))
        found.extend(global_findings(src))
        found.extend(fence_findings(src))
        found.extend(order_findings(src))
        found.extend(dist_findings(src, names))
        found.extend(platform_findings(src))
        found.extend(throw_findings(src))
        found.extend(noexcept_findings(src))
    found.extend(layer_findings(sources, layers))
    found.extend(cmake_findings(root, RUNTIME_DIR + "/tools"))
    scripts = 0
    for rel, full in _walk(root, SCRIPTS_DIR, (".sh",)):
        text = _read(full)
        if text is not None:
            scripts += 1
            found.extend(output_findings(rel, text))
    allowed = {(e["path"], e["rule"], e["symbol"]) for e in entries}
    kept = set()
    for rel, line, rule, symbol, message in found:
        if (rel, rule, symbol) in allowed or (rel, rule, "*") in allowed:
            continue
        kept.add((rel, line, rule, message))
    errors.extend("%s:%d: %s: %s" % item for item in sorted(kept))
    return errors, len(sources), scripts


def main(argv):
    root, allow, args = ROOT, None, argv[1:]
    while args:
        if args[0] == "--root" and len(args) >= 2:
            root = os.path.abspath(args[1])
        elif args[0] == "--allow" and len(args) >= 2:
            allow = os.path.abspath(args[1])
        else:
            print("usage: runtime_lint.py [--root DIR] [--allow FILE]", file=sys.stderr)
            return 2
        args = args[2:]
    errors, files, scripts = check_tree(root, allow)
    for e in errors:
        print(e)
    if errors:
        print("runtime-lint: FAIL (%d finding(s) in %d C++ file(s) and %d script(s))" % (len(errors), files, scripts))
        return 1
    print("runtime-lint: ok (%d C++ file(s), %d script(s) scanned)" % (files, scripts))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
