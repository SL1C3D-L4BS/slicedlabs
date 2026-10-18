"""ci-yaml-check (WO-028): structural rules for .github/workflows/*.yml and .github/dependabot.yml. Stdlib only.

Usage: python3 -I tools/ci/check_workflows.py [--strict] [--root DIR]
Output: one line per finding, `RULE-ID path:line message`; exit 1 when an error exists, else 0.
Rules (errors unless marked warning):
  R-CI-PARSE            the file does not parse with tools/ci/miniyaml.py
  R-CI-USES-UNPINNED    a `uses:` ref is not a full 40-hex commit sha (docker:// needs @sha256:<64 hex>)
  R-CI-USES-COMMENT     a pinned `uses:` line has no `# vX.Y.Z` comment
  R-CI-UNVERIFIED-SHA   warning (error with --strict): a sha is a placeholder (one repeated character) or the line
                        says UNVERIFIED; the founder resolves it before the first run
  R-CI-PERMISSIONS      no top-level `permissions`, or any permissions value other than read / none
  R-CI-TRIGGER          no `on`, `pull_request_target` used, or `workflow_dispatch` missing
  R-CI-JOB              a job id or `runs-on` outside the allowed list
  R-CI-BUILD-ENTRY      a run line builds or tests other than via scripts/ci-local.sh, `cmake --preset <configure
                        preset>` or `cmake --workflow --preset <workflow preset>` (presets read from CMakePresets.json)
  R-CI-INJECT           `${{ github.event.* }}` or `github.head_ref` expanded inside a run script
  R-CI-SECRET           a secret other than GITHUB_TOKEN is referenced
  R-CI-DEPENDABOT       .github/dependabot.yml lacks version 2, the github-actions ecosystem or a monthly interval
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import miniyaml  # noqa: E402

ALLOWED_JOBS = {"linux": "ubuntu-24.04", "windows": "windows-latest"}
USES_RE = re.compile(r"^\s*-?\s*uses:\s*(\S+)(.*)$")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
DOCKER_RE = re.compile(r"^docker://\S+@sha256:[0-9a-f]{64}$")
VERSION_COMMENT = re.compile(r"#\s*v\d+(\.\d+)*")
SHA256_ASSIGN = re.compile(r"_SHA256\s*[:=]\s*[\"']?([0-9A-Za-z-]+)")
BUILD_WORDS = ("ninja", "make", "ctest", "msbuild", "gradle", "bazel")
INJECT_RE = re.compile(r"\$\{\{[^}]*(github\.event\.|github\.head_ref)")
SECRET_RE = re.compile(r"secrets\.([A-Za-z0-9_]+)")


def load_presets(root):
    path = os.path.join(root, "CMakePresets.json")
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError):
        return None, None
    configure = {p["name"] for p in data.get("configurePresets", []) if not p.get("hidden")}
    workflow = {p["name"] for p in data.get("workflowPresets", [])}
    return configure, workflow


def is_placeholder(sha):
    return len(set(sha)) == 1


def check_uses(text, rel, findings):
    for number, line in enumerate(text.splitlines(), 1):
        match = USES_RE.match(line)
        if not match:
            continue
        ref, tail = match.group(1).strip("\"'"), match.group(2)
        if ref.startswith("./"):
            continue
        where = f"{rel}:{number}"
        if ref.startswith("docker://"):
            if not DOCKER_RE.match(ref):
                findings.append(("R-CI-USES-UNPINNED", where, f"{ref} is not pinned to @sha256:<64 hex>"))
            continue
        sha = ref.rpartition("@")[2] if "@" in ref else ""
        if not SHA_RE.match(sha):
            findings.append(("R-CI-USES-UNPINNED", where, f"{ref} is not pinned to a full 40-hex commit sha"))
            continue
        if not VERSION_COMMENT.search(tail):
            findings.append(("R-CI-USES-COMMENT", where, f"{ref} has no `# vX.Y.Z` comment"))
        if is_placeholder(sha) or "UNVERIFIED" in tail:
            findings.append(("R-CI-UNVERIFIED-SHA", where, f"{ref} is a placeholder or marked UNVERIFIED"))


def check_sha256_pins(text, rel, findings):
    for number, line in enumerate(text.splitlines(), 1):
        match = SHA256_ASSIGN.search(line)
        if not match:
            continue
        value = match.group(1)
        if not re.fullmatch(r"[0-9a-f]{64}", value) or is_placeholder(value) or "UNVERIFIED" in line:
            findings.append(("R-CI-UNVERIFIED-SHA", f"{rel}:{number}", "a tarball sha256 pin is a placeholder"))


def check_permissions(perms, where, findings, top):
    if perms is None:
        if top:
            findings.append(("R-CI-PERMISSIONS", where, "no top-level `permissions` block"))
        return
    if isinstance(perms, str):
        if perms != "read-all":
            findings.append(("R-CI-PERMISSIONS", where, f"permissions: {perms} is not read-only"))
        return
    for scope, level in perms.items():
        if str(level) not in ("read", "none"):
            findings.append(("R-CI-PERMISSIONS", where, f"permissions.{scope}: {level} is not read or none"))


def check_triggers(doc, rel, findings):
    triggers = doc.get("on")
    if triggers is None:
        findings.append(("R-CI-TRIGGER", rel, "no `on:` block"))
        return
    names = {str(triggers)} if isinstance(triggers, str) else set(triggers)
    if "pull_request_target" in names:
        findings.append(("R-CI-TRIGGER", rel, "pull_request_target is not allowed"))
    if "workflow_dispatch" not in names:
        findings.append(("R-CI-TRIGGER", rel, "workflow_dispatch is missing"))


def build_violation(command, configure, workflow):
    """Return a message when one shell line builds or tests outside the allowed entry points, else None."""
    words = command.split()
    if not words:
        return None
    first = os.path.basename(words[0])
    if words[1:] == ["--version"]:
        return None
    if first == "bash" and len(words) > 1 and words[1].endswith("scripts/ci-local.sh"):
        return None
    if first in BUILD_WORDS:
        return f"`{first}` called directly"
    if first == "just" and len(words) > 1 and not words[1].startswith("-"):
        return "`just <recipe>` called directly (use scripts/ci-local.sh)"
    if first == "cmake":
        if "--build" in words or "--install" in words:
            return "`cmake --build/--install` called directly"
        name = words[words.index("--preset") + 1] if "--preset" in words[:-1] else ""
        if "--workflow" in words:
            if workflow is None or name not in workflow:
                return f"`cmake --workflow --preset {name}`: no such workflow preset"
        elif "--preset" in words:
            if configure is None or name not in configure:
                return f"`cmake --preset {name}`: no such configure preset"
    return None


def check_run(script, rel, configure, workflow, findings):
    where = f"{rel}:{getattr(script, 'line', 0)}"
    if INJECT_RE.search(script):
        findings.append(("R-CI-INJECT", where, "an event field is expanded inside a run script"))
    for command in script.replace("\\\n", " ").splitlines():
        for part in re.split(r"&&|\|\||;|\|", command):
            message = build_violation(part.strip(), configure, workflow)
            if message:
                findings.append(("R-CI-BUILD-ENTRY", where, message))


def load_yaml(path, rel, findings):
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    try:
        return text, miniyaml.loads(text)
    except miniyaml.YamlError as err:
        findings.append(("R-CI-PARSE", rel, str(err)))
        return text, None


def check_workflow(path, rel, configure, workflow, findings):
    text, doc = load_yaml(path, rel, findings)
    if doc is None:
        return
    if not isinstance(doc, dict):
        findings.append(("R-CI-PARSE", rel, "top level is not a mapping"))
        return
    check_uses(text, rel, findings)
    check_sha256_pins(text, rel, findings)
    check_triggers(doc, rel, findings)
    check_permissions(doc.get("permissions"), rel, findings, True)
    for secret in sorted(set(SECRET_RE.findall(text)) - {"GITHUB_TOKEN"}):
        findings.append(("R-CI-SECRET", rel, f"secrets.{secret} is referenced"))
    jobs = doc.get("jobs")
    if not isinstance(jobs, dict) or not jobs:
        findings.append(("R-CI-JOB", rel, "no jobs"))
        return
    for job_id, job in jobs.items():
        where = f"{rel}:{jobs.lines.get(job_id, 0)}"
        if job_id not in ALLOWED_JOBS:
            findings.append(("R-CI-JOB", where, f"job id {job_id!r} is not one of {sorted(ALLOWED_JOBS)}"))
        if not isinstance(job, dict):
            findings.append(("R-CI-JOB", where, "job is not a mapping"))
            continue
        runner = str(job.get("runs-on", ""))
        if runner not in ALLOWED_JOBS.values():
            findings.append(("R-CI-JOB", where, f"runs-on {runner!r} is not one of {sorted(ALLOWED_JOBS.values())}"))
        check_permissions(job.get("permissions"), where, findings, False)
        for step in job.get("steps") or []:
            if isinstance(step, dict) and "run" in step:
                check_run(step["run"], rel, configure, workflow, findings)


def check_dependabot(path, rel, findings):
    _, doc = load_yaml(path, rel, findings)
    if doc is None:
        return
    updates = doc.get("updates") if isinstance(doc, dict) else None
    good = False
    for entry in updates or []:
        if isinstance(entry, dict) and entry.get("package-ecosystem") == "github-actions":
            schedule = entry.get("schedule")
            good = isinstance(schedule, dict) and schedule.get("interval") == "monthly"
    if not isinstance(doc, dict) or doc.get("version") != "2" or not good:
        findings.append(("R-CI-DEPENDABOT", rel, "need version 2 and a github-actions entry with a monthly interval"))


def run(root, strict):
    findings = []
    configure, workflow = load_presets(root)
    if configure is None:
        findings.append(("R-CI-BUILD-ENTRY", "CMakePresets.json", "cannot read the preset names"))
    wf_dir = os.path.join(root, ".github", "workflows")
    names = sorted(n for n in os.listdir(wf_dir) if n.endswith((".yml", ".yaml"))) if os.path.isdir(wf_dir) else []
    if not names:
        findings.append(("R-CI-JOB", ".github/workflows", "no workflow files"))
    for name in names:
        check_workflow(os.path.join(wf_dir, name), f".github/workflows/{name}", configure, workflow, findings)
    dep = os.path.join(root, ".github", "dependabot.yml")
    if os.path.isfile(dep):
        check_dependabot(dep, ".github/dependabot.yml", findings)
    else:
        findings.append(("R-CI-DEPENDABOT", ".github/dependabot.yml", "missing"))
    errors = 0
    for rule, where, message in findings:
        warning = rule == "R-CI-UNVERIFIED-SHA" and not strict
        errors += 0 if warning else 1
        print(f"{rule} {where} {'warning: ' if warning else ''}{message}")
    print(f"ci-yaml-check: {len(names)} workflow(s), {errors} error(s), {len(findings) - errors} warning(s)")
    return 1 if errors else 0


def main(argv):
    root = os.getcwd()
    strict = False
    args = list(argv)
    while args:
        arg = args.pop(0)
        if arg == "--strict":
            strict = True
        elif arg == "--root" and args:
            root = args.pop(0)
        else:
            print("usage: check_workflows.py [--strict] [--root DIR]", file=sys.stderr)
            return 2
    return run(root, strict)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
