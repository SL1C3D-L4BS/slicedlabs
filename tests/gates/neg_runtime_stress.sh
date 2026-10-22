#!/usr/bin/env bash
# neg-runtime-stress (WO-098, SL-ARCH-003 ARCH3-R057, R060, R162, ADR-0024): the stress watchdog must catch a lost
# wakeup. Copies engine/runtime/src/sync_event_count.cpp to build/runtime/neg/stress/ twice: one unmodified (the
# control) and one with the `// neg-lost-wakeup` line deleted (WO-080 marks the epoch increment of notify_one with it;
# when the marker is missing, the first `.fetch_add(` line of notify_one is deleted instead and the run says so, a
# deviation to record). Each copy is compiled with a planted ping-pong driver: two threads hand a token back and forth
# through two event_counts 20000 times under a watchdog that reports "stress watchdog: no completion" and exits 3 when
# no hand-off completes for 5 s.
#   control: exit 0 and no watchdog report; planted: a non-zero exit or the watchdog report, within 60 s.
# A missing clang++, a failed compile or a timeout of the outer 60 s fails the negative test.
# Works under build/runtime/neg/stress (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
work="$build/runtime/neg/stress"
source="$root/engine/runtime/src/sync_event_count.cpp"
marker="// neg-lost-wakeup"

trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-runtime-stress: FAIL: $1"
  exit 1
}

command -v clang++ >/dev/null 2>&1 || fail "clang++ not found"
[ -f "$source" ] || fail "$source not found"

rm -rf "$work"
mkdir -p "$work"
cp "$source" "$work/event_count_control.cpp"
marks="$(grep -cF "$marker" "$source" || true)"
if [ "$marks" -gt 0 ]; then
  grep -vF "$marker" "$source" >"$work/event_count_planted.cpp" || true
  echo "neg-runtime-stress: deleted $marks '$marker' line in the copy"
else
  # Deviation (WO-098): no marker; delete the first .fetch_add( line inside notify_one.
  awk 'BEGIN { inside = 0; done = 0 }
       /event_count::notify_one\(\)/ { inside = 1 }
       inside && !done && /\.fetch_add\(/ { done = 1; next }
       inside && /^}/ { inside = 0 }
       { print }' "$source" >"$work/event_count_planted.cpp"
  echo "neg-runtime-stress: DEVIATION: no '$marker' marker; deleted the first .fetch_add( of notify_one"
fi
if cmp -s "$work/event_count_control.cpp" "$work/event_count_planted.cpp"; then
  fail "the planted copy equals the original"
fi

# The planted driver: ping and pong hand a token back and forth; each wait is prepare_wait, a check, then wait.
cat >"$work/driver.cpp" <<'EOF'
#include "sl/runtime/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

constexpr std::uint64_t rounds = 20000;
constexpr auto watchdog_limit = std::chrono::milliseconds(5000);

std::atomic<std::uint64_t> progress{0};
std::atomic<bool> finished{false};

void await(sl::runtime::event_count &ec, const std::atomic<std::uint64_t> &value, std::uint64_t want) {
  while (value.load() < want) {
    const auto key = ec.prepare_wait();
    if (value.load() >= want) {
      ec.cancel_wait();
      return;
    }
    ec.wait(key);
  }
}

} // namespace

int main() {
  sl::runtime::event_count to_pong;
  sl::runtime::event_count to_ping;
  std::atomic<std::uint64_t> served{0};
  std::atomic<std::uint64_t> returned{0};
  std::thread watchdog([] {
    std::uint64_t seen = progress.load();
    auto last = std::chrono::steady_clock::now();
    while (!finished.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      const std::uint64_t now_seen = progress.load();
      const auto now = std::chrono::steady_clock::now();
      if (now_seen != seen) {
        seen = now_seen;
        last = now;
      } else if (now - last > watchdog_limit) {
        std::printf("stress watchdog: no completion within %lld ms after %llu hand-offs\n",
                    static_cast<long long>(watchdog_limit.count()), static_cast<unsigned long long>(seen));
        std::fflush(stdout);
        std::_Exit(3);
      }
    }
  });
  std::thread pong([&] {
    for (std::uint64_t i = 1; i <= rounds; ++i) {
      await(to_pong, served, i);
      returned.store(i);
      to_ping.notify_one();
      progress.fetch_add(1);
    }
  });
  for (std::uint64_t i = 1; i <= rounds; ++i) {
    served.store(i);
    to_pong.notify_one();
    await(to_ping, returned, i);
    progress.fetch_add(1);
  }
  pong.join();
  finished.store(true);
  watchdog.join();
  std::printf("driver: %llu hand-offs\n", static_cast<unsigned long long>(progress.load()));
  return 0;
}
EOF

flags=(-std=c++23 -g -O1 -pthread -I "$root/engine/runtime/include" -I "$root/engine/core/include"
  -DSL_RUNTIME_FAULTS=0)

# build_and_run <variant>: sets out, status and wall (seconds)
build_and_run() {
  local variant="$1"
  if ! clang++ "${flags[@]}" "$work/driver.cpp" "$work/event_count_$variant.cpp" -o "$work/stress_$variant" \
    >"$work/compile_$variant.log" 2>&1; then
    head -n 20 "$work/compile_$variant.log"
    fail "$variant: compile failed"
  fi
  local start=$SECONDS
  set +e
  out="$(timeout 60 "$work/stress_$variant" 2>&1)"
  status=$?
  set -e
  wall=$((SECONDS - start))
  [ "$status" -ne 124 ] || fail "$variant: no failure and no watchdog report within 60 s"
}

build_and_run control
if [ "$status" -ne 0 ] || grep -q "stress watchdog" <<<"$out"; then
  echo "$out" | head -n 20
  fail "control: the unmodified event_count must complete (exit $status)"
fi
echo "neg-runtime-stress: control -> exit 0 in ${wall} s, $(tail -n 1 <<<"$out")"

build_and_run planted
if [ "$status" -eq 0 ] && ! grep -q "stress watchdog" <<<"$out"; then
  echo "$out" | head -n 20
  fail "planted: the lost wakeup went unnoticed (exit 0, no watchdog report)"
fi
echo "neg-runtime-stress: planted -> exit $status in ${wall} s: $(grep -m1 "stress watchdog" <<<"$out" || echo "a failure")"
echo "neg-runtime-stress: PASS"
