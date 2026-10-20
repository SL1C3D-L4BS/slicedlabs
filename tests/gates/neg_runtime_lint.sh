#!/usr/bin/env bash
# neg-runtime-lint (WO-077, ARCH3-R009): runtime-lint must fail on a planted defect of each rule and name it.
#   control: runtime-lint passes on a copy of engine/runtime, docs/arch/SL-ARCH-003-*.md, scripts/runtime and
#            tools/runtime/lint_allow.json
#   then, in a fresh copy each time, one file with the defect on line 3 per rule (13 rules): the lint must exit non-zero
#   and print `<planted file>:3: <RULE>:`
# Works under build/runtime/neg/lint (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
work="$build/runtime/neg/lint"
checker="$root/tools/runtime/runtime_lint.py"

trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-runtime-lint: FAIL: $1"
  exit 1
}

# a fresh copy of everything the lint reads
fresh() {
  rm -rf "$work"
  mkdir -p "$work/docs/arch" "$work/engine" "$work/scripts" "$work/tools/runtime"
  cp -r "$root/engine/runtime" "$work/engine/runtime"
  cp "$root"/docs/arch/SL-ARCH-003-*.md "$work/docs/arch/"
  cp -r "$root/scripts/runtime" "$work/scripts/runtime"
  cp "$root/tools/runtime/lint_allow.json" "$work/tools/runtime/"
}

# write_file <relative path> <line 1> <line 2> <line 3>: the third line carries the defect
write_file() {
  mkdir -p "$(dirname "$work/$1")"
  printf '%s\n%s\n%s\n' "$2" "$3" "$4" >"$work/$1"
}

# expect_fail <rule> <relative path>: the lint exits non-zero and names the rule, the file and line 3
expect_fail() {
  local rule="$1" file="$2" out status
  set +e
  out="$(python3 -I "$checker" --root "$work" 2>&1)"
  status=$?
  set -e
  [ "$status" -ne 0 ] || fail "$rule: runtime-lint exited 0 with the planted $file"
  if ! grep -qF "$file:3: $rule: " <<<"$out"; then
    echo "$out" | head -n 6
    fail "$rule: no finding starts with '$file:3: $rule: '"
  fi
  echo "neg-runtime-lint: $rule -> exit $status, $file:3 named"
}

echo "neg-runtime-lint: control run (copy of the working tree)"
fresh
if ! out="$(python3 -I "$checker" --root "$work" 2>&1)"; then
  echo "$out" | head -n 8
  fail "control: runtime-lint must exit 0 on the unmodified copy"
fi

src="engine/runtime/src"
inc="engine/runtime/include/sl/runtime"

fresh
write_file "$inc/error.hpp" "#pragma once" "// planted: an L0 header including an L3 header" '#include "sl/runtime/jobs.hpp"'
expect_fail R-RT-LAYER "$inc/error.hpp"

fresh
write_file "$src/zz_neg_fabric.cpp" "// planted" "// workspace.hpp is for testing/ and tools/ only" '#include "sl/fabric/workspace.hpp"'
expect_fail R-RT-FABRIC "$src/zz_neg_fabric.cpp"

fresh
write_file "$src/zz_neg_txcall.cpp" "// planted" "// src/ must not name the transaction manager" 'void neg(sl::fabric::transaction_manager &tm);'
expect_fail R-RT-TXCALL "$src/zz_neg_txcall.cpp"

fresh
write_file "$src/zz_neg_math.cpp" "// planted" "// no mathematics types before Volume 02" 'struct vec3 { float x, y, z; };'
expect_fail R-RT-MATH "$src/zz_neg_math.cpp"

fresh
write_file "$src/zz_neg_global.cpp" "// planted" "// mutable namespace-scope state" 'int neg_counter = 0;'
expect_fail R-RT-GLOBAL "$src/zz_neg_global.cpp"

fresh
write_file "$src/zz_neg_fence.cpp" "// planted" "// no standalone fences" 'void neg() { std::atomic_thread_fence(std::memory_order_seq_cst); }'
expect_fail R-RT-FENCE "$src/zz_neg_fence.cpp"

fresh
write_file "$src/zz_neg_order.cpp" "// planted" "// a weak order needs a pairs comment" 'void neg(std::atomic<int> &a) { a.store(1, std::memory_order_release); }'
expect_fail R-RT-ORDER "$src/zz_neg_order.cpp"

fresh
write_file "$src/zz_neg_dist.cpp" "// planted" "// no implementation-defined randomness" 'void neg() { std::random_device rd; }'
expect_fail R-RT-DIST "$src/zz_neg_dist.cpp"

fresh
write_file "$src/zz_neg_platform.cpp" "// planted" "// the OS only through the platform" 'void neg() { auto t = std::chrono::steady_clock::now(); }'
expect_fail R-RT-PLATFORM "$src/zz_neg_platform.cpp"

fresh
write_file "$src/zz_neg_throw.cpp" "// planted" "// errors are values" 'void neg() { throw 1; }'
expect_fail R-RT-THROW "$src/zz_neg_throw.cpp"

fresh
write_file "$src/zz_neg_noexcept.cpp" "// planted" "// job bodies are noexcept" 'void neg(job_system &js) { js.submit(job_desc{.fn = [](job_context &) { }}); }'
expect_fail R-RT-NOEXCEPT "$src/zz_neg_noexcept.cpp"

fresh
write_file "engine/runtime/tools/zz_neg/CMakeLists.txt" "# planted" "# the demo never links the fabric testing library" 'target_link_libraries(neg PRIVATE sl_fabric_testing)'
expect_fail R-RT-LINK "engine/runtime/tools/zz_neg/CMakeLists.txt"

fresh
write_file "scripts/runtime/zz-neg-output.sh" "#!/usr/bin/env bash" "# planted: a script writing outside build/" 'echo planted >/tmp/neg-runtime-lint.out'
expect_fail R-RT-OUTPUT "scripts/runtime/zz-neg-output.sh"

echo "neg-runtime-lint: ok (control exit 0, 13 planted defects each failed naming the rule and file:line)"
