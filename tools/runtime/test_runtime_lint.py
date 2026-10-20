"""Tests for runtime_lint (WO-077): a clean fixture tree, one planted defect per rule, the scoped fabric allow-lists.

Fixtures are temp trees built from strings (no subprocess): a copy of the SL-ARCH-003 document (its `layers` table),
the repository's lint_allow.json and the files of one case. A planted line carries the word PLANT; the expected
finding is `<path>:<that line>: <RULE>:`.
"""

import contextlib
import glob
import importlib.util
import io
import json
import os
import shutil
import tempfile
import unittest

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("runtime_lint", os.path.join(_here, "runtime_lint.py"))
rl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rl)

REPO = os.path.dirname(os.path.dirname(_here))
DOC = glob.glob(os.path.join(REPO, "docs", "arch", "SL-ARCH-003-*.md"))[0]
ALLOW = os.path.join(_here, "lint_allow.json")
RT = "engine/runtime/"

CLEAN = {
    RT + "include/sl/runtime/error.hpp": """#pragma once
#include <cstdint>
#include "sl/fabric/value.hpp"
namespace sl::runtime {
inline constexpr std::uint32_t first_code = 6000;  // a throw in a comment is not code
constexpr const char *const names[] = {"try", "catch"};
struct error_info {
  std::uint32_t code;
  static constexpr int limit = 8;
  static const int other = 3;
};
}  // namespace sl::runtime
""",
    RT + "include/sl/runtime/faults.hpp": """#pragma once
#include "sl/runtime/error.hpp"
#include "sl/fabric/id.hpp"
""",
    RT + "include/sl/runtime/sync.hpp": """#pragma once
#include "sl/runtime/memory.hpp"
""",
    RT + "include/sl/runtime/memory.hpp": "#pragma once\n#include \"sl/runtime/error.hpp\"\n",
    RT + "include/sl/runtime/jobs.hpp": """#pragma once
#include "sl/runtime/sync.hpp"
#include "sl/runtime/error.hpp"
namespace sl::runtime::jobs {
inline void go(job_system &js) {
  js.submit(job_desc{.fn = [](job_context &) noexcept { }});
  std::sort(v.begin(), v.end(), [](int a, int b) { return a < b; });
  auto [a, b] = pair;
  int c = arr[2];
}
}
""",
    RT + "src/platform_clock.cpp": """#include <chrono>
#include <thread>
#include <filesystem>
#include "sl/runtime/error.hpp"
void sample() {
  auto t = std::chrono::steady_clock::now();
  std::thread worker;
  std::size_t h = std::hash<int>{}(3);
}
""",
    RT + "src/platform_files.cpp": '#include "sl/fabric/vfs.hpp"\n#include <fstream>\n',
    RT + "src/platform_files_posix.cpp": '#include "sl/fabric/vfs.hpp"\n',
    RT + "src/sync.cpp": """#include <atomic>
void f(std::atomic<int> &x) {
  x.store(1, std::memory_order_release);  // pairs: g
  // pairs: g
  x.load(std::memory_order_acquire);
  x.fetch_add(1);
}
""",
    RT + "src/faults.cpp": """#include <array>
#include <atomic>
namespace {
std::array<std::atomic<int>, 11> g_registry{};
}
""",
    RT + "src/tables.cpp": """#include <array>
#include <string_view>
namespace sl::runtime {
namespace {
constexpr std::array<std::string_view, 2> kNames{{"a", "b"}};
const std::string_view kOne = "one";
inline constexpr int kMax = 4;
struct holder { static constexpr int n = 1; int member = 0; };
enum class color : int { red, green };
int helper(int v) { static constexpr int k = 3; return v + k; }
template <class T> inline constexpr bool is_ok = true;
}  // namespace
class widget {
 public:
  widget() : a_{1} { }
  bool operator<(const widget &o) const noexcept { return a_ < o.a_; }
 private:
  int a_;
};
}  // namespace sl::runtime
""",
    RT + "testing/fixture.cpp": '#include "sl/fabric/workspace.hpp"\n#include "sl/fabric/transaction.hpp"\n',
    RT + "tools/demo.cpp": '#include "sl/fabric/authority.hpp"\n#include "sl/fabric/boundary.hpp"\n',
    RT + "tools/CMakeLists.txt": "add_executable(sl_runtime_demo demo.cpp)\n"
                                 "# never sl_fabric_testing here\n"
                                 "target_link_libraries(sl_runtime_demo PRIVATE sl_runtime)\n",
    "scripts/runtime/ok.sh": """#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
log="$build/runtime/san/x.log"
mkdir -p "$build/runtime/san"
cmake --preset debug >"$build/runtime/configure.log" 2>&1
ctest --preset debug 2>&1 | tee "$log"
python3 -I tool.py --evidence-dir build/runtime/evidence -o "$build/out.json"
grep -o foo file.txt >/dev/null 2>&1
cat <<'EOF' > /dev/null
echo hi > /etc/not-a-script
EOF
""",
}


@contextlib.contextmanager
def tree(files=None, base=True, allow=True):
    """A temp root: the SL-ARCH-003 document, the allow-list and the clean files, then `files` on top."""
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "docs", "arch"))
        shutil.copy(DOC, os.path.join(tmp, "docs", "arch"))
        if allow:
            os.makedirs(os.path.join(tmp, "tools", "runtime"))
            shutil.copy(ALLOW, os.path.join(tmp, "tools", "runtime", "lint_allow.json"))
        merged = dict(CLEAN) if base else {}
        merged.update(files or {})
        for rel, text in merged.items():
            if text is None:
                continue
            path = os.path.join(tmp, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text)
        yield tmp


def lint(files=None, **kw):
    with tree(files, **kw) as root:
        return rl.check_tree(root)[0]


def planted_line(text):
    for n, line in enumerate(text.split("\n"), start=1):
        if "PLANT" in line:
            return n
    raise AssertionError("no PLANT marker")


class RuntimeLintTest(unittest.TestCase):
    def assertFinding(self, errors, path, text, rule):
        want = "%s:%d: %s: " % (path, planted_line(text), rule)
        self.assertTrue(any(e.startswith(want) for e in errors), "no finding starts with %r in:\n%s" % (
            want, "\n".join(errors)))

    def plant(self, rule, path, text, others=None):
        files = {path: text}
        files.update(others or {})
        errors = lint(files)
        self.assertFinding(errors, path, text, rule)
        return errors

    # --- RT-INV-LINT-01 ---------------------------------------------------------------------------------------

    def test_rt_inv_lint_01_clean_tree_passes(self):
        self.assertEqual(lint(), [])
        with tree() as root:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(rl.main(["runtime_lint.py", "--root", root]), 0)
            self.assertIn("runtime-lint: ok", out.getvalue())

    def test_rt_inv_lint_01_layer(self):
        text = '#pragma once\n#include "sl/runtime/jobs.hpp"  // PLANT: L0 includes L3\n'
        errors = self.plant("R-RT-LAYER", RT + "include/sl/runtime/error.hpp", text)
        self.assertIn("higher layer", "\n".join(errors))
        cycle = {RT + "include/sl/runtime/sync.hpp": '#include "sl/runtime/allocators.hpp"  // PLANT\n',
                 RT + "include/sl/runtime/allocators.hpp": '#include "sl/runtime/sync.hpp"\n'}
        errors = lint(cycle)
        self.assertFinding(errors, RT + "include/sl/runtime/sync.hpp", cycle[RT + "include/sl/runtime/sync.hpp"],
                           "R-RT-LAYER")
        self.assertTrue(any("cycle" in e for e in errors), errors)
        both = {RT + "include/sl/runtime/runtime.hpp": '#include "sl/runtime/bridge.hpp"  // PLANT\n'}
        self.assertFinding(lint(both), RT + "include/sl/runtime/runtime.hpp",
                           both[RT + "include/sl/runtime/runtime.hpp"], "R-RT-LAYER")
        ok = {RT + "include/sl/runtime/frame.hpp": '#include "sl/runtime/jobs.hpp"\n#include "sl/runtime/error.hpp"\n'}
        self.assertEqual(lint(ok), [])

    def test_rt_inv_lint_01_fabric(self):
        text = '#include "sl/fabric/internal/codec.hpp"  // PLANT\n'
        self.plant("R-RT-FABRIC", RT + "src/job.cpp", text)
        text = '#include "sl/fabric/memory_backend.hpp"  // PLANT\n'
        self.plant("R-RT-FABRIC", RT + "include/sl/runtime/memory.hpp", text)

    def test_rt_inv_lint_01_txcall(self):
        text = "namespace n {\nvoid f(sl::fabric::transaction_manager &tm);  // PLANT\n}\n"
        self.plant("R-RT-TXCALL", RT + "include/sl/runtime/memory.hpp", text)
        self.assertEqual(lint({RT + "testing/x.cpp": "void f(transaction_manager &tm);\n"}), [])

    def test_rt_inv_lint_01_math(self):
        text = "namespace n {\nstruct vec3 { float x, y, z; };  // PLANT\n}\n"
        self.plant("R-RT-MATH", RT + "src/geometry.cpp", text)
        text = "#include <immintrin.h>  // PLANT\n"
        self.plant("R-RT-MATH", RT + "src/simd_user.cpp", text)
        text = "using quat_t = float;  // PLANT\n"
        self.plant("R-RT-MATH", RT + "src/q.cpp", text)
        text = "class fixed_stepper {};\nstruct material_slot {};\nstruct transformer {};\n"
        self.assertEqual(lint({RT + "include/sl/runtime/time.hpp": text}), [])

    def test_rt_inv_lint_01_global(self):
        for body in ("int counter = 0;  // PLANT\n", "std::atomic<int> hits;  // PLANT\n",
                     "static int count_;  // PLANT\n", "thread_local int depth = 0;  // PLANT\n",
                     "constinit int boot = 3;  // PLANT\n", "const char *name = \"x\";  // PLANT\n"):
            self.plant("R-RT-GLOBAL", RT + "src/state.cpp", "namespace sl::runtime {\nnamespace {\n" + body + "}\n}\n")
        text = "int f() {\n  static int calls = 0;  // PLANT\n  return ++calls;\n}\n"
        self.plant("R-RT-GLOBAL", RT + "src/state.cpp", text)
        text = "struct s {\n  static inline int shared = 0;  // PLANT\n};\n"
        self.plant("R-RT-GLOBAL", RT + "include/sl/runtime/memory.hpp", text)
        # the allow-list excuses the failpoint registry of faults.cpp and nothing else
        errors = lint({RT + "src/faults.cpp": "int rogue;  // PLANT\n"}, allow=False)
        self.assertFinding(errors, RT + "src/faults.cpp", "int rogue;  // PLANT\n", "R-RT-GLOBAL")
        self.assertEqual(lint({RT + "src/faults.cpp": "int rogue;\n"}), [])
        errors = lint({RT + "src/other.cpp": "int rogue;  // PLANT\n"})
        self.assertFinding(errors, RT + "src/other.cpp", "int rogue;  // PLANT\n", "R-RT-GLOBAL")

    def test_rt_inv_lint_01_fence(self):
        for body in ("std::atomic_thread_fence(std::memory_order_seq_cst);", "volatile int flag;", "asm(\"nop\");",
                     "__asm__ __volatile__(\"\");", "syscall(SYS_futex, 0);"):
            self.plant("R-RT-FENCE", RT + "src/fence.cpp", "void f() {\n  " + body + "  // PLANT\n}\n")

    def test_rt_inv_lint_01_order(self):
        for order in ("std::memory_order_relaxed", "std::memory_order_acq_rel", "std::memory_order::acquire"):
            text = "void f(std::atomic<int> &x) {\n  x.store(1, %s);  // PLANT\n}\n" % order
            self.plant("R-RT-ORDER", RT + "src/order.cpp", text)
        text = "void f(std::atomic<int> &x) {\n  // pairs: reader\n  x.store(1, std::memory_order_release);\n}\n"
        self.assertEqual(lint({RT + "src/order.cpp": text}), [])
        text = ("void f(std::atomic<int> &x) {\n  // pairs: reader\n\n"
                "  x.store(1, std::memory_order_release);  // PLANT\n}\n")
        self.plant("R-RT-ORDER", RT + "src/order.cpp", text)

    def test_rt_inv_lint_01_dist(self):
        for body in ("std::uniform_int_distribution<int> d(0, 3);", "std::random_device rd;", "int r = rand();",
                     "srand(3);", "auto h = std::hash<int>{}(1);"):
            self.plant("R-RT-DIST", RT + "src/rng.cpp", "void f() {\n  " + body + "  // PLANT\n}\n")
        text = ("#include <unordered_map>\nstruct s {\n  std::unordered_map<int, int> table_;\n"
                "  void f() {\n    for (auto &kv : table_) {  // PLANT\n    }\n  }\n};\n")
        self.plant("R-RT-DIST", RT + "src/unordered.cpp", text)
        text = ("void f(const std::map<int, int> &table_) {\n  for (auto &kv : table_) {\n  }\n"
                "  for (int i = 0; i < 3; ++i) {\n  }\n}\n")
        self.assertEqual(lint({RT + "src/ordered.cpp": text}), [])

    def test_rt_inv_lint_01_platform(self):
        for body in ("auto t = std::chrono::steady_clock::now();", "std::thread t;", "std::jthread t;",
                     "FILE *f = fopen(\"x\", \"r\");", "int fd = ::open(\"x\", 0);", "namespace fs = std::filesystem;",
                     "std::ifstream in;", "std::ofstream out;"):
            self.plant("R-RT-PLATFORM", RT + "src/job.cpp", "void f() {\n  " + body + "  // PLANT\n}\n")
        text = ("void f(std::thread::id who, const std::filesystem::path &p) "
                "{ auto n = std::thread::hardware_concurrency(); }\n")
        self.assertEqual(lint({RT + "src/threads.cpp": text}), [])

    def test_rt_inv_lint_01_throw(self):
        for body in ("throw 1;", "try {", "} catch (...) {", "int v = m.at(3);", "int v = std::stoi(s);"):
            self.plant("R-RT-THROW", RT + "src/err.cpp", "void f() {\n  " + body + "  // PLANT\n}\n")

    def test_rt_inv_lint_01_noexcept(self):
        text = "void f(job_system &js) {\n  js.submit(job_desc{.fn = [](job_context &) {  // PLANT\n  }});\n}\n"
        self.plant("R-RT-NOEXCEPT", RT + "src/lam.cpp", text)
        text = "void f(frame_scheduler &fs) {\n  fs.add_task(\"t\", [&] { run(); });  // PLANT\n}\n"
        self.plant("R-RT-NOEXCEPT", RT + "src/lam.cpp", text)
        text = "void f(job_system &js) {\n  js.submit(job_desc{.fn = [](job_context &) noexcept {\n  }});\n}\n"
        self.assertEqual(lint({RT + "src/lam.cpp": text}), [])

    def test_rt_inv_lint_01_link(self):
        for name in ("sl_fabric_testing", "sl::fabric_testing"):
            text = ("add_executable(sl_runtime_demo demo.cpp)\n"
                    "target_link_libraries(sl_runtime_demo PRIVATE %s)  # PLANT\n" % name)
            self.plant("R-RT-LINK", RT + "tools/CMakeLists.txt", text)
        text = "target_link_libraries(sl_runtime_testing PUBLIC sl::fabric_testing)\n"
        self.assertEqual(lint({RT + "testing/CMakeLists.txt": text}), [])

    def test_rt_inv_lint_01_output(self):
        for body in ("echo hi > /tmp/x.log", "echo hi >> out.txt", "cmake -o result.bin x.c",
                     "tool --evidence-dir evidence", "mkdir -p out/dir",
                     "cp a.txt /tmp/b.txt", "echo x | tee ../log.txt", "tool 2>errors.log"):
            self.plant("R-RT-OUTPUT", "scripts/runtime/bad.sh", "#!/usr/bin/env bash\n" + body + "  # PLANT\n")
        text = '#!/usr/bin/env bash\nwork="$HOME/x"\nmkdir -p "$work"  # PLANT\n'
        self.plant("R-RT-OUTPUT", "scripts/runtime/bad.sh", text)

    def test_rt_inv_lint_01_comments_and_strings_are_blanked(self):
        text = ('/* throw volatile std::thread */\nconst char *const kMsg = "throw try catch";\n'
                'const auto raw = R"x(throw volatile)x";  // asm\n// std::chrono::steady_clock::now()\n')
        self.assertEqual(lint({RT + "src/blank.cpp": text}), [])

    def test_rt_inv_lint_01_repository_tables_load(self):
        entries, errors = rl.load_allow(ALLOW)
        self.assertEqual(errors, [])
        self.assertEqual([(e["path"], e["rule"]) for e in entries], [("engine/runtime/src/faults.cpp", "R-RT-GLOBAL")])
        rank, names, flat, errs = rl.load_layers(REPO)
        self.assertEqual(errs, [])
        self.assertEqual(names, ["L%d" % i for i in range(10)])
        self.assertEqual(rank["error.hpp"], 0)
        self.assertEqual(rank["ecs/world.hpp"], 5)
        self.assertEqual(flat, {9})

    def test_rt_inv_lint_01_allow_list_errors(self):
        bad = {"allow": [{"path": "engine/runtime/src/faults.cpp", "rule": "R-RT-GLOBAL", "symbol": "*", "reason": " "},
                         {"path": "a", "rule": "R-RT-NOPE", "symbol": "*", "reason": "x"}, {"path": "a"}]}
        with tree() as root:
            path = os.path.join(root, "tools", "runtime", "lint_allow.json")
            with open(path, "w", encoding="utf-8") as fh:
                json.dump(bad, fh)
            errors = rl.check_tree(root)[0]
        self.assertEqual(len([e for e in errors if "R-RT-ALLOW" in e]), 3, errors)
        self.assertTrue(any("has no reason" in e for e in errors))

    def test_rt_inv_lint_01_usage_and_failure_exit(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(rl.main(["runtime_lint.py", "--nope"]), 2)
        files = {RT + "src/err.cpp": "void f() { throw 1; }\n"}
        with tree(files) as root:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(rl.main(["runtime_lint.py", "--root", root]), 1)
            self.assertIn("engine/runtime/src/err.cpp:1: R-RT-THROW: ", out.getvalue())
            self.assertIn("runtime-lint: FAIL (1 finding(s)", out.getvalue())

    # --- RT-INV-LINT-02 ---------------------------------------------------------------------------------------

    def test_rt_inv_lint_02_workspace_is_scoped(self):
        inc = '#include "sl/fabric/workspace.hpp"  // PLANT\n'
        for path in ("include/sl/runtime/memory.hpp", "src/job.cpp", "src/platform_files.cpp"):
            self.plant("R-RT-FABRIC", RT + path, inc)
        for path in ("testing/fixture2.cpp", "tools/demo2.cpp", "testing/deep/more.hpp"):
            self.assertEqual(lint({RT + path: inc}), [], path)
        for name in ("transaction.hpp", "authority.hpp"):
            line = '#include "sl/fabric/%s"  // PLANT\n' % name
            self.plant("R-RT-FABRIC", RT + "src/job.cpp", line)
            self.assertEqual(lint({RT + "tools/x.cpp": line}), [])

    def test_rt_inv_lint_02_vfs_only_in_platform_files(self):
        inc = '#include "sl/fabric/vfs.hpp"  // PLANT\n'
        for path in ("src/platform_files.cpp", "src/platform_files_posix.cpp"):
            self.assertEqual(lint({RT + path: inc}), [], path)
        for path in ("src/platform_clock.cpp", "src/job.cpp", "include/sl/runtime/memory.hpp", "testing/fixture2.cpp",
                     "tools/demo2.cpp", "src/sub/platform_files.cpp"):
            self.plant("R-RT-FABRIC", RT + path, inc)

    def test_rt_inv_lint_02_base_list_and_never_list(self):
        for name in rl.FABRIC_BASE:
            inc = '#include "sl/fabric/%s"\n' % name
            for path in ("include/sl/runtime/memory.hpp", "src/job.cpp", "testing/t.cpp", "tools/t.cpp"):
                self.assertEqual(lint({RT + path: inc}), [], (name, path))
        for name in ("store.hpp", "f5.hpp", "internal/codec.hpp"):
            inc = '#include <sl/fabric/%s>  // PLANT\n' % name
            for path in ("include/sl/runtime/memory.hpp", "src/job.cpp", "testing/t.cpp", "tools/t.cpp"):
                self.plant("R-RT-FABRIC", RT + path, inc)


if __name__ == "__main__":
    unittest.main()
