"""A minimal YAML reader for the GitHub workflow files of this repository (WO-028). Stdlib only.

Supported: block mappings, block lists (including `- key: value` items and lists at the key's own indent), plain and
quoted scalars, `|` and `>` block scalars (kept verbatim), inline `[a, b]` lists and `{}` / `{a: b}` maps of scalars,
full-line and trailing comments. Not supported (raises YamlError): anchors, tags, multi-line plain scalars, documents.
Scalars come back as `Str` (a str with `.line`), mappings as `Map` (a dict with `.lines`), lists as `list`.
"""

import re


class YamlError(Exception):
    def __init__(self, message, line):
        super().__init__(f"line {line}: {message}")
        self.line = line


class Str(str):
    line = 0


class Map(dict):
    def __init__(self):
        super().__init__()
        self.lines = {}


def _str(text, line):
    value = Str(text)
    value.line = line
    return value


def _strip_comment(text):
    """Drop a trailing ` # comment` that is outside quotes."""
    quote = ""
    for pos, char in enumerate(text):
        if quote:
            if char == quote:
                quote = ""
        elif char in "\"'":
            quote = char
        elif char == "#" and (pos == 0 or text[pos - 1] in " \t"):
            return text[:pos].rstrip()
    return text.rstrip()


def _split_key(text):
    """Return (key, rest) when text is `key: rest` or `key:`, else None."""
    quote = ""
    for pos, char in enumerate(text):
        if quote:
            if char == quote:
                quote = ""
        elif char in "\"'":
            quote = char
        elif char == ":" and (pos + 1 == len(text) or text[pos + 1] in " \t"):
            key = text[:pos].strip()
            if len(key) >= 2 and key[0] == key[-1] and key[0] in "\"'":
                key = key[1:-1]
            return key, text[pos + 1:].strip()
        elif char in "[{" and pos == 0:
            return None
    return None


class _Reader:
    def __init__(self, text):
        self.lines = []
        for number, raw in enumerate(text.splitlines(), 1):
            if "\t" in raw[: len(raw) - len(raw.lstrip())]:
                raise YamlError("tab in indentation", number)
            self.lines.append([len(raw) - len(raw.lstrip(" ")), raw.strip(), number, raw])
        self.pos = 0

    def skip_blank(self):
        while self.pos < len(self.lines):
            text = self.lines[self.pos][1]
            if text and not text.startswith("#") and text != "---":
                return
            self.pos += 1

    def peek(self):
        self.skip_blank()
        return self.lines[self.pos] if self.pos < len(self.lines) else None

    def parse(self):
        first = self.peek()
        if first is None:
            return Map()
        value = self.node(first[0])
        if self.peek() is not None:
            raise YamlError("unexpected content", self.peek()[2])
        return value

    def node(self, indent):
        item = self.peek()
        if item[1] == "-" or item[1].startswith("- "):
            return self.sequence(indent)
        return self.mapping(indent)

    def scalar(self, text, number):
        text = _strip_comment(text)
        if text.startswith("["):
            if not text.endswith("]"):
                raise YamlError("multi-line flow list is not supported", number)
            inner = text[1:-1].strip()
            return [self.scalar(part.strip(), number) for part in inner.split(",")] if inner else []
        if text.startswith("{"):
            if not text.endswith("}"):
                raise YamlError("multi-line flow map is not supported", number)
            result = Map()
            inner = text[1:-1].strip()
            for part in inner.split(",") if inner else []:
                pair = _split_key(part.strip())
                if pair is None:
                    raise YamlError("bad flow map entry", number)
                result[pair[0]] = self.scalar(pair[1], number)
                result.lines[pair[0]] = number
            return result
        if text[:1] in "&*!":
            raise YamlError("anchors, aliases and tags are not supported", number)
        if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
            text = text[1:-1]
        return _str(text, number)

    def block_scalar(self, indent, number, style):
        parts = []
        base = None
        while self.pos < len(self.lines):
            line_indent, text, _, raw = self.lines[self.pos]
            if text and line_indent <= indent:
                break
            if text and base is None:
                base = line_indent
            parts.append(raw[base:] if text and base is not None else "")
            self.pos += 1
        while parts and not parts[-1]:
            parts.pop()
        joiner = "\n" if style.startswith("|") else " "
        return _str(joiner.join(parts) + ("\n" if style.startswith("|") else ""), number)

    def value(self, rest, indent, number):
        """The value after `key:` or `- `, where rest is the text on the same line."""
        if rest.startswith(("|", ">")):
            return self.block_scalar(indent, number, rest)
        rest = _strip_comment(rest)
        if rest:
            return self.scalar(rest, number)
        nxt = self.peek()
        if nxt is None:
            return None
        if nxt[0] > indent or (nxt[0] == indent and (nxt[1] == "-" or nxt[1].startswith("- "))):
            return self.node(nxt[0])
        return None

    def mapping(self, indent):
        result = Map()
        while True:
            item = self.peek()
            if item is None or item[0] < indent:
                return result
            if item[0] > indent:
                raise YamlError("unexpected indentation", item[2])
            if item[1] == "-" or item[1].startswith("- "):
                return result
            pair = _split_key(_strip_comment(item[1]))
            if pair is None:
                raise YamlError("expected `key: value`", item[2])
            key, rest = pair
            if key in result:
                raise YamlError(f"duplicate key {key!r}", item[2])
            self.pos += 1
            result[key] = self.value(rest, indent, item[2])
            result.lines[key] = item[2]

    def sequence(self, indent):
        result = []
        while True:
            item = self.peek()
            if item is None or item[0] < indent:
                return result
            if item[0] > indent:
                raise YamlError("unexpected indentation", item[2])
            if not (item[1] == "-" or item[1].startswith("- ")):
                return result
            content = item[1][1:].lstrip()
            offset = len(item[3]) - len(item[3].lstrip(" ")) + (len(item[1]) - len(content))
            if not content or content.startswith("#"):
                self.pos += 1
                nxt = self.peek()
                result.append(self.node(nxt[0]) if nxt is not None and nxt[0] > indent else None)
            elif _split_key(_strip_comment(content)) is not None and not content.startswith(("[", "{", "\"", "'")):
                item[0], item[1] = offset, content
                result.append(self.mapping(offset))
            else:
                self.pos += 1
                result.append(self.value(content, indent, item[2]))


def loads(text):
    return _Reader(text).parse()


_BARE = re.compile(r"^[A-Za-z0-9_.-]+$")


def is_bare(text):
    return bool(_BARE.match(text))
