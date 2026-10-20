#!/usr/bin/env bash
# neg-runtime-poison (WO-081, SL-ARCH-003 ARCH3-R046, R048, ADR-0023): AddressSanitizer must catch a read of arena
# memory after reset().
#   configures the ci-asan preset and builds sl_runtime, writes a planted program under build/runtime/neg/poison/ (an
#   arena_resource allocation, reset(), then a read through the old pointer) and compiles it with the ci-asan compiler
#   and flags (taken from build/ci-asan/compile_commands.json) against build/ci-asan's sl_runtime, sl_fabric and
#   sl_core.
#   control (the read before reset): exit 0 and no AddressSanitizer output.
#   planted (the read after reset): exit non-zero and "AddressSanitizer: use-after-poison".
#   A missing build, a failed compile or AddressSanitizer failing to start fails the negative test.
# Works under build/runtime/neg/poison (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
work="$build/runtime/neg/poison"
asan="$root/build/ci-asan"

trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-runtime-poison: FAIL: $1"
  exit 1
}

cd "$root"
cmake --preset ci-asan >/dev/null || fail "cmake --preset ci-asan failed"
cmake --build --preset ci-asan --target sl_runtime >/dev/null || fail "building sl_runtime (ci-asan) failed"
for lib in engine/runtime/libsl_runtime.a engine/fabric/libsl_fabric.a engine/core/libsl_core.a; do
  [ -f "$asan/$lib" ] || fail "$asan/$lib not found"
done
[ -f "$asan/compile_commands.json" ] || fail "$asan/compile_commands.json not found"

rm -rf "$work"
mkdir -p "$work"

# The compiler and flags of an sl_runtime source in the ci-asan tree, one argument per line.
python3 -I - "$asan/compile_commands.json" "$root/engine/runtime/src/alloc_arena.cpp" >"$work/flags.txt" <<'EOF'
import json, shlex, sys
entries = json.load(open(sys.argv[1]))
for e in entries:
    if e["file"] == sys.argv[2]:
        args = e.get("arguments") or shlex.split(e["command"])
        out, skip = [], False
        for a in args:
            if skip:
                skip = False
                continue
            if a in ("-o", "-c"):
                skip = True
                continue
            out.append(a)
        print("\n".join(out))
        sys.exit(0)
sys.exit("no compile command for " + sys.argv[2])
EOF
mapfile -t compile <"$work/flags.txt"
[ "${#compile[@]}" -gt 1 ] || fail "no ci-asan compile command for alloc_arena.cpp"
case " ${compile[*]} " in
  *" -fsanitize=address "*) ;;
  *) fail "the ci-asan compile command has no -fsanitize=address" ;;
esac

# The planted program: argv[1] "control" reads before reset(), "planted" after it.
cat >"$work/poison.cpp" <<'EOF'
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memory.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::puts("usage: poison control|planted");
    return 2;
  }
  const bool planted = std::string_view(argv[1]) == "planted";
  sl::runtime::mem::tracking_resource upstream("upstream");
  sl::runtime::mem::arena_resource arena(upstream, 4096);
  auto got = arena.try_allocate(64, 16, arena.default_tag());
  if (!got.has_value()) {
    std::puts("poison: try_allocate failed");
    return 2;
  }
  auto *p = static_cast<unsigned char *>(*got);
  std::memset(p, 0x5A, 64);
  int seen = 0;
  if (!planted) {
    seen = p[8];
  }
  arena.reset();
  if (planted) {
    seen = p[8]; // use after reset: the range is poisoned
  }
  std::printf("poison: read %d\n", seen);
  return 0;
}
EOF

if ! "${compile[@]}" "$work/poison.cpp" -o "$work/poison" -fsanitize=address \
  "$asan/engine/runtime/libsl_runtime.a" "$asan/engine/fabric/libsl_fabric.a" "$asan/engine/core/libsl_core.a" \
  -lstdc++exp >"$work/compile.log" 2>&1; then
  head -n 20 "$work/compile.log"
  fail "the planted program did not compile"
fi
echo "neg-runtime-poison: compiled the planted program with the ci-asan flags"

# run <variant>: sets out and status
run() {
  set +e
  out="$(ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:exitcode=66" timeout 60 "$work/poison" "$1" 2>&1)"
  status=$?
  set -e
  [ "$status" -ne 124 ] || fail "$1: the program timed out after 60 s"
  if grep -qE "AddressSanitizer failed|Shadow memory range interleaves|ERROR: AddressSanitizer: (failed|CHECK)" <<<"$out"; then
    echo "$out" | head -n 6
    fail "$1: AddressSanitizer failed to start"
  fi
}

run control
if [ "$status" -ne 0 ] || grep -q "AddressSanitizer" <<<"$out"; then
  echo "$out" | head -n 20
  fail "control: the read before reset must run clean (exit $status)"
fi
grep -q "poison: read 90" <<<"$out" || fail "control: the program did not read its pattern"
echo "neg-runtime-poison: control -> exit 0, no AddressSanitizer report"

run planted
[ "$status" -ne 0 ] || fail "planted: the read after reset exited 0"
grep -q "AddressSanitizer: use-after-poison" <<<"$out" || {
  echo "$out" | head -n 20
  fail "planted: no 'AddressSanitizer: use-after-poison'"
}
echo "neg-runtime-poison: planted -> exit $status, AddressSanitizer use-after-poison reported"
echo "neg-runtime-poison: PASS"
