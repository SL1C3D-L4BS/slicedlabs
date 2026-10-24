"""check_toolchain (WO-130): the pinned toolchain of the hosted linux job, toolchain/ci-linux.json. Stdlib only.

Usage:
  python3 -I tools/ci/check_toolchain.py packages <ci-linux.json>
      print the pinned package names, one per line (the job installs exactly these)
  python3 -I tools/ci/check_toolchain.py verify <ci-linux.json> <pacman-Q-output>
      compare `pacman -Q` output (written by the job to a file; no subprocess here) with the pinned versions;
      one line per mismatch, exit 1 when any package is missing or differs, 2 on a bad pin file
"""

import json
import re
import sys

DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
DATE_RE = re.compile(r"^\d{4}/\d{2}/\d{2}$")
NAME_RE = re.compile(r"^[a-z0-9@._+][a-z0-9@._+-]*$")
SERVER_FMT = "https://archive.archlinux.org/repos/{}/$repo/os/$arch"


def load_pin(path):
    """Return (pin, problems): the parsed pin file and a list of problems with it (empty when well formed)."""
    try:
        with open(path, encoding="utf-8") as handle:
            pin = json.load(handle)
    except (OSError, ValueError) as err:
        return None, [f"cannot read {path}: {err}"]
    problems = []
    image = pin.get("image") if isinstance(pin, dict) else None
    archive = pin.get("archive") if isinstance(pin, dict) else None
    packages = pin.get("packages") if isinstance(pin, dict) else None
    if not isinstance(pin, dict) or pin.get("schema") != "sl-ci-linux/1":
        problems.append("schema is not sl-ci-linux/1")
    if not isinstance(image, dict) or not all(isinstance(image.get(k), str) and image.get(k) for k in ("name", "tag")):
        problems.append("image needs name and tag")
    elif not DIGEST_RE.match(str(image.get("digest", ""))) or len(set(image["digest"][7:])) == 1:
        problems.append("image.digest is not a real sha256:<64 hex>")
    if not isinstance(archive, dict) or not DATE_RE.match(str(archive.get("date", ""))):
        problems.append("archive.date is not YYYY/MM/DD")
    elif archive.get("server") != SERVER_FMT.format(archive["date"]):
        problems.append("archive.server does not match archive.date")
    if not isinstance(packages, dict) or not packages:
        problems.append("packages is empty")
    else:
        for name, version in packages.items():
            if not NAME_RE.match(name) or not isinstance(version, str) or not version.strip() or " " in version:
                problems.append(f"packages.{name}: bad name or version")
    return (None if problems else pin), problems


def parse_pacman_q(text):
    installed = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2:
            installed[parts[0]] = parts[1]
    return installed


def mismatches(pin, installed):
    out = []
    for name, version in sorted(pin["packages"].items()):
        have = installed.get(name)
        if have is None:
            out.append(f"{name}: not installed, pinned {version}")
        elif have != version:
            out.append(f"{name}: installed {have}, pinned {version}")
    return out


def main(argv):
    if len(argv) == 2 and argv[0] == "packages":
        pin, problems = load_pin(argv[1])
        if pin is None:
            print("\n".join(f"check_toolchain: {p}" for p in problems), file=sys.stderr)
            return 2
        print("\n".join(sorted(pin["packages"])))
        return 0
    if len(argv) == 3 and argv[0] == "verify":
        pin, problems = load_pin(argv[1])
        if pin is None:
            print("\n".join(f"check_toolchain: {p}" for p in problems), file=sys.stderr)
            return 2
        try:
            with open(argv[2], encoding="utf-8") as handle:
                installed = parse_pacman_q(handle.read())
        except OSError as err:
            print(f"check_toolchain: cannot read {argv[2]}: {err}", file=sys.stderr)
            return 2
        bad = mismatches(pin, installed)
        for line in bad:
            print(f"check_toolchain: {line}")
        count = len(pin["packages"])
        print(f"check_toolchain: {count - len(bad)} of {count} pinned packages match (snapshot {pin['archive']['date']})")
        return 1 if bad else 0
    print("usage: check_toolchain.py packages <json> | verify <json> <pacman-Q-file>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
