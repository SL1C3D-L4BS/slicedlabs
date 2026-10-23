#!/usr/bin/env bash
# neg-runtime-determinism (WO-099, SL-ARCH-003 ARCH3-R085 and R170, ADR-0026): the D1 check must catch command
# buffers played back in completion order.
#   copies engine/runtime/src/ecs_command_queue.cpp to build/runtime/neg/det/ and replaces its merge_ordered playback
#   (the line WO-091 marks `// neg-completion-order`) with a playback that ignores the merge keys: the buffers in
#   reverse open order, rotated by a hash of a process-wide flush serial (WO-114, D-099-15). It differs from the merge
#   order by construction, not by thread timing, and a resumed or replayed pass meets other rotations at the same tick
#   than the fresh pass, so the workers=1 row diverges on one CPU as well; compiles the copy with the debug preset's
#   flags and links sl_runtime_demo with the
#   patched object ahead of libsl_runtime.a; runs the demo with workers 1 and 8 (parallel, 5000 lanterns, 120 ticks)
#   and requires tools/runtime/determinism_check.py to exit 1 naming the divergence.
#   A missing marker, a failed compile or link, a run without hashes.json or a checker exit other than 1 fails.
# Works under build/runtime/neg/det (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
build="${SL_BUILD_DIR:-$root/build}"
debug="$build/debug"
work="$build/runtime/neg/det"
source="$root/engine/runtime/src/ecs_command_queue.cpp"
marker="// neg-completion-order"
playback='{ static std::uint64_t neg_flush = 0; std::uint64_t h = ++neg_flush * 0x9E3779B97F4A7C15ULL;'
playback+=' h ^= h >> 29U; const std::size_t n = in.size(); const std::size_t r = n == 0 ? 0 : h % n;'
playback+=' for (std::size_t j = 0; j < n; ++j) { auto *b = in[n - 1 - ((j + r) % n)];'
playback+=' for (auto &e : b->entries()) { player(std::as_const(e.key), e.value); } } }'

fail() {
  echo "neg-runtime-determinism: FAIL: $1"
  exit 1
}

[ -f "$source" ] || fail "$source not found"
marked="$(grep -cF "$marker" "$source" || true)"
[ "$marked" -eq 1 ] || fail "ecs_command_queue.cpp has $marked '$marker' lines, expected 1"

cmake --preset debug >/dev/null
cmake --build --preset debug --target sl_runtime_demo

rm -rf "$work"
mkdir -p "$work/runs"
patched="$work/ecs_command_queue.cpp"
python3 -I - "$source" "$patched" "$marker" "$playback" <<'EOF'
import sys
src, dst, marker, playback = sys.argv[1:5]
lines = open(src, encoding="utf-8").read().split("\n")
hits = [i for i, l in enumerate(lines) if marker in l]
indent = lines[hits[0]][: len(lines[hits[0]]) - len(lines[hits[0]].lstrip())]
lines[hits[0]] = indent + playback + " // neg: keyless playback"
open(dst, "w", encoding="utf-8").write("\n".join(lines))
EOF
grep -qF "merge_ordered(in, player)" "$patched" && fail "the copy still plays back through merge_ordered"
grep -qF "$playback" "$patched" || fail "the copy lacks the keyless playback"
echo "neg-runtime-determinism: replaced the '$marker' playback in the copy"

# The compile and link commands of the debug build, pointed at the copy and at build/runtime/neg/det.
python3 -I - "$debug" "$source" "$patched" "$work" <<'EOF'
import json, os, shlex, subprocess, sys
debug, source, patched, work = sys.argv[1:5]
entry = next(e for e in json.load(open(os.path.join(debug, "compile_commands.json"))) if e["file"] == source)
args = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
obj = os.path.join(work, "ecs_command_queue.cpp.o")
out, skip = [], False
for a in args:
    if skip:
        skip = False
        continue
    if a in ("-o", "-MF", "-MT"):
        skip = True
        continue
    if a in ("-MD", "-MMD"):
        continue
    out.append(patched if a == source else a)
out += ["-o", obj]
if subprocess.call(out, cwd=entry["directory"]) != 0:
    sys.exit("compile failed")
link = subprocess.run(["ninja", "-C", debug, "-t", "commands", "engine/runtime/tools/sl_runtime_demo"],
                      check=True, capture_output=True, text=True).stdout.strip().split("\n")[-1]
segment = next(s for s in link.split("&&") if " -o " in s)
parts = [a for a in shlex.split(segment) if not a.startswith("-Wl,--dependency-file")
         and not a.startswith("--dependency-file")]
exe = os.path.join(work, "sl_runtime_demo")
lib = next(i for i, a in enumerate(parts) if a.endswith("libsl_runtime.a"))
parts.insert(lib, obj)
o = parts.index("-o")
parts[o + 1] = exe
if subprocess.call(parts, cwd=debug) != 0:
    sys.exit("link failed")
EOF
[ -x "$work/sl_runtime_demo" ] || fail "the patched demo was not linked"
echo "neg-runtime-determinism: linked $work/sl_runtime_demo"

for w in 1 8; do
  dir="$work/runs/neg-w$w-parallel"
  set +e
  "$work/sl_runtime_demo" --workers "$w" --mode parallel --entities 5000 --ticks 120 --snapshot-every 60 \
    --evidence-dir "$dir" >"$work/run-w$w.log" 2>&1
  code=$?
  set -e
  echo "neg-runtime-determinism: workers $w exit $code"
  [ -f "$dir/hashes.json" ] || fail "workers $w wrote no hashes.json (exit $code)"
done

set +e
python3 -I tools/runtime/determinism_check.py --runs "$work/runs" --primary neg --workers 1,8 --modes parallel \
  --out "$work/hash-matrix.json" | tee "$work/check.log"
code=${PIPESTATUS[0]}
set -e
[ "$code" -eq 1 ] || fail "determinism_check exit $code with keyless playback, expected 1"
grep -q "divergence at workers=" "$work/check.log" || fail "determinism_check did not name a divergence"
# The workers=1 rows come first in matrix order: naming workers=1 shows the divergence needs no second thread.
grep -q "divergence at workers=1 " "$work/check.log" || fail "the workers=1 row did not diverge"
echo "neg-runtime-determinism: ok (keyless playback diverges)"
