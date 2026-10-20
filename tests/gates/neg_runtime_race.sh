#!/usr/bin/env bash
# neg-runtime-race (WO-082, SL-ARCH-003 ARCH3-R070, ADR-0024): ThreadSanitizer must catch a work queue without its lock.
#   copies engine/runtime/src/queue_locked.cpp to build/runtime/neg/race/ twice: one unmodified (the control), one with
#   every `// neg-race-lock` line deleted; compiles each with a planted owner-plus-thieves driver under
#   clang++ -fsanitize=thread and runs it.
#   control: exit 0 and no ThreadSanitizer output; stripped: exit non-zero and "WARNING: ThreadSanitizer: data race".
#   A missing clang++, a failed compile or ThreadSanitizer failing to start fails the negative test.
# Works under build/runtime/neg/race (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
work="$build/runtime/neg/race"
source="$root/engine/runtime/src/queue_locked.cpp"
marker="// neg-race-lock"

trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-runtime-race: FAIL: $1"
  exit 1
}

command -v clang++ >/dev/null 2>&1 || fail "clang++ not found"
[ -f "$source" ] || fail "$source not found"

rm -rf "$work"
mkdir -p "$work"
cp "$source" "$work/queue_control.cpp"
grep -vF "$marker" "$source" >"$work/queue_stripped.cpp" || true

locks="$(grep -cF "$marker" "$work/queue_control.cpp" || true)"
[ "$locks" -gt 0 ] || fail "queue_locked.cpp has no '$marker' line"
left="$(grep -cF "$marker" "$work/queue_stripped.cpp" || true)"
[ "$left" -eq 0 ] || fail "the stripped copy still has $left '$marker' lines"
if cmp -s "$work/queue_control.cpp" "$work/queue_stripped.cpp"; then
  fail "the stripped copy equals the original"
fi
echo "neg-runtime-race: deleted $locks '$marker' lines in the copy"

# The planted driver: one owner pushes and pops (LIFO) while three thieves steal (FIFO); every item is counted.
cat >"$work/driver.cpp" <<'EOF'
#include "sl/runtime/work_queue.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

int main() {
  auto made = sl::runtime::jobs::make_locked_queue(256);
  if (!made) {
    std::puts("driver: make_locked_queue failed");
    return 2;
  }
  sl::runtime::jobs::work_queue &q = **made;
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> taken{0};
  std::vector<std::thread> thieves;
  for (int t = 0; t < 3; ++t) {
    thieves.emplace_back([&] {
      while (!done.load()) {
        if (q.steal()) {
          taken.fetch_add(1);
        }
      }
    });
  }
  for (std::uint32_t i = 0; i < 20000; ++i) {
    if (!q.push(i) || i % 3 == 0) {
      if (q.pop()) {
        taken.fetch_add(1);
      }
    }
  }
  done.store(true);
  for (auto &t : thieves) {
    t.join();
  }
  std::printf("driver: taken %llu\n", static_cast<unsigned long long>(taken.load()));
  return 0;
}
EOF

flags=(-std=c++23 -fsanitize=thread -g -O1 -I "$root/engine/runtime/include" -I "$root/engine/core/include"
  -DSL_RUNTIME_FAULTS=0)

# build_and_run <variant>: sets out and status
build_and_run() {
  local variant="$1"
  if ! clang++ "${flags[@]}" "$work/driver.cpp" "$work/queue_$variant.cpp" -o "$work/race_$variant" \
    >"$work/compile_$variant.log" 2>&1; then
    head -n 20 "$work/compile_$variant.log"
    fail "$variant: compile failed"
  fi
  set +e
  out="$(TSAN_OPTIONS="halt_on_error=0 exitcode=66" timeout 60 "$work/race_$variant" 2>&1)"
  status=$?
  set -e
  if grep -q "FATAL: ThreadSanitizer" <<<"$out"; then
    echo "$out" | head -n 6
    fail "$variant: ThreadSanitizer failed to start"
  fi
  [ "$status" -ne 124 ] || fail "$variant: the driver timed out after 60 s"
}

build_and_run control
if [ "$status" -ne 0 ] || grep -q "ThreadSanitizer" <<<"$out"; then
  echo "$out" | head -n 20
  fail "control: the unmodified queue must run clean (exit $status)"
fi
echo "neg-runtime-race: control -> exit 0, no ThreadSanitizer report"

build_and_run stripped
[ "$status" -ne 0 ] || fail "stripped: the driver exited 0 without the lock"
grep -q "WARNING: ThreadSanitizer: data race" <<<"$out" || {
  echo "$out" | head -n 20
  fail "stripped: no 'WARNING: ThreadSanitizer: data race'"
}
echo "neg-runtime-race: stripped -> exit $status, ThreadSanitizer data race reported"
echo "neg-runtime-race: PASS"
