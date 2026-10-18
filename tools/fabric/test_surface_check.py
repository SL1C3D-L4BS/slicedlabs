"""Tests for the F5 public-surface lint (WO-051, FAB-INV-F5-03): the real headers pass, each rule fires on a plant."""

import contextlib
import importlib.util
import io
import os
import shutil
import tempfile
import unittest

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("surface_check", os.path.join(_here, "surface_check.py"))
sc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sc)

ROOT = os.path.dirname(os.path.dirname(_here))

CAPABILITY_OK = """
namespace sl::fabric {
class transaction_manager;
class mutation_capability;
class mutation_capability {
public:
  mutation_capability(const mutation_capability &) = delete;
  mutation_capability(mutation_capability &&) = delete;
  mutation_capability &operator=(const mutation_capability &) = delete;
  mutation_capability &operator=(mutation_capability &&) = delete;
  ~mutation_capability() = default;

private:
  constexpr mutation_capability() noexcept = default;
  friend class transaction_manager;
};
struct store_backend {
  virtual int commit(const mutation_capability &cap, int batch) noexcept = 0;
};
}
"""

VALUE_OK = """
#include <span>
#include "sl/fabric/bytes.hpp"
namespace sl::fabric {
class value {
public:
  // value &comment_only();  -- comments never count
  [[nodiscard]] const value &payload() const noexcept;
  [[nodiscard]] std::span<const value> items() const noexcept;
  [[nodiscard]] const value *find_field(int no) const noexcept;
  [[nodiscard]] static value uint(int v) noexcept;
  value &operator=(const value &) = default;
  [[nodiscard]] result<value> with(int p, value v) const;
  friend bool operator==(const value &a, const value &b) noexcept;

private:
  value &internal_ref();
  std::shared_ptr<const int> node_;
};
class cache {
  mutable std::mutex mutex_;
  mutable std::atomic<int> hits_{0};
};
}
"""


def rules(findings):
    return sorted({f[1] for f in findings})


def line_of(text, needle):
    return [i + 1 for i, line in enumerate(text.splitlines()) if needle in line][0]


class SurfaceCheckTest(unittest.TestCase):
    def test_fab_inv_f5_03_surface(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = sc.main(["surface_check.py", "--root", ROOT])
        self.assertEqual(status, 0, out.getvalue())
        self.assertIn("fabric-surface-check: ok", out.getvalue())

    def test_fab_inv_f5_03_clean_texts(self):
        self.assertEqual(sc.lint_text(VALUE_OK), [])
        friends = sc.lint_text(CAPABILITY_OK)
        self.assertEqual([(f[1], f[2]) for f in friends],
                         [("R-F5-FRIEND", "mutation_capability:transaction_manager")])

    def test_fab_inv_f5_03_setter(self):
        planted = VALUE_OK.replace("  [[nodiscard]] static value",
                                   "  value &at_mut();\n  void set_kind(int k) noexcept;\n"
                                   "  value *raw() noexcept;\n  std::span<value> items_mut();\n"
                                   "  [[nodiscard]] static value")
        found = sc.lint_text(planted)
        self.assertEqual(rules(found), ["R-F5-SETTER"])
        self.assertEqual(sorted(f[2] for f in found),
                         ["value::at_mut", "value::items_mut", "value::raw", "value::set_kind"])
        at = [f for f in found if f[2] == "value::at_mut"][0]
        self.assertEqual(at[0], line_of(planted, "at_mut"))

    def test_fab_inv_f5_03_setter_only_on_data_classes_and_public(self):
        other = VALUE_OK.replace("class value {", "class builder {").replace("const value &payload", "value &payload")
        self.assertEqual(sc.lint_text(other), [])
        snap = "class snapshot {\npublic:\n  void set_root(int d);\n};\n"
        self.assertEqual([(f[0], f[1]) for f in sc.lint_text(snap)], [(3, "R-F5-SETTER")])
        view = "struct record_view {\n  int &body();\n};\n"
        self.assertEqual(rules(sc.lint_text(view)), ["R-F5-SETTER"])

    def test_fab_inv_f5_03_mutable(self):
        planted = VALUE_OK.replace("  mutable std::mutex mutex_;",
                                   "  mutable std::mutex mutex_;\n  mutable int hits = 0;")
        found = sc.lint_text(planted)
        self.assertEqual([(f[1], f[2]) for f in found], [("R-F5-MUTABLE", "cache::hits")])
        lam = "struct s {\n  int f() const { auto g = [x = 1]() mutable { return ++x; }; return g(); }\n};\n"
        self.assertEqual(sc.lint_text(lam), [])

    def test_fab_inv_f5_03_friend(self):
        planted = CAPABILITY_OK.replace("  friend class transaction_manager;",
                                        "  friend class transaction_manager;\n  friend struct rogue;\n"
                                        "  friend void forge(int);")
        symbols = sorted(f[2] for f in sc.lint_text(planted) if f[1] == "R-F5-FRIEND")
        self.assertEqual(symbols, ["mutation_capability:forge", "mutation_capability:rogue",
                                   "mutation_capability:transaction_manager"])

    def test_fab_inv_f5_03_capability(self):
        copyable = CAPABILITY_OK.replace("  mutation_capability(const mutation_capability &) = delete;\n", "")
        self.assertIn(("R-F5-CAP", "mutation_capability::copy_constructor"),
                      [(f[1], f[2]) for f in sc.lint_text(copyable)])
        public_ctor = CAPABILITY_OK.replace("  ~mutation_capability() = default;",
                                            "  ~mutation_capability() = default;\n  mutation_capability() = default;")
        self.assertIn("R-F5-CAP", rules(sc.lint_text(public_ctor)))
        by_value = CAPABILITY_OK.replace("struct store_backend {",
                                         "mutation_capability forge();\nstruct store_backend {")
        cap = [f for f in sc.lint_text(by_value) if f[1] == "R-F5-CAP"]
        self.assertEqual([f[0] for f in cap], [line_of(by_value, "forge()")])
        by_ref = CAPABILITY_OK.replace("const mutation_capability &cap", "mutation_capability &cap")
        self.assertIn("R-F5-CAP", rules(sc.lint_text(by_ref)))

    def test_fab_inv_f5_03_include(self):
        planted = VALUE_OK.replace('#include "sl/fabric/bytes.hpp"',
                                   '#include "sl/fabric/bytes.hpp"\n#include "sl/fabric/internal/node.hpp"')
        found = sc.lint_text(planted)
        self.assertEqual([(f[0], f[1]) for f in found], [(line_of(planted, "internal/"), "R-F5-INCLUDE")])

    def test_fab_inv_f5_03_tree_and_allow_list(self):
        work = tempfile.mkdtemp()
        try:
            inc = os.path.join(work, *sc.HEADER_DIR.split("/"))
            os.makedirs(os.path.join(inc, "internal"))
            os.makedirs(os.path.join(work, "tools", "fabric"))
            with open(os.path.join(inc, "store.hpp"), "w", encoding="utf-8") as fh:
                fh.write(CAPABILITY_OK)
            with open(os.path.join(inc, "internal", "hidden.hpp"), "w", encoding="utf-8") as fh:
                fh.write("class value { public: value &at_mut(); mutable int x; };\n")  # internal/ is not public
            allow = os.path.join(work, "tools", "fabric", "surface-allow.txt")
            entry = "%s/store.hpp:R-F5-FRIEND:mutation_capability:transaction_manager" % sc.HEADER_DIR

            def run(lines):
                with open(allow, "w", encoding="utf-8") as fh:
                    fh.write("\n".join(lines) + "\n")
                return sc.check_tree(work)

            errors, files, allowed = run(["# comment", entry + " C4 (ADR-0016)"])
            self.assertEqual((errors, files, allowed), ([], 1, 1))
            errors, _, _ = run([])
            self.assertEqual(len(errors), 1)
            self.assertIn("store.hpp:15: R-F5-FRIEND: class mutation_capability declares friend transaction_manager",
                          errors[0])
            errors, _, _ = run([entry])
            self.assertTrue(any("R-F5-ALLOW" in e and "no reason" in e for e in errors), errors)
            errors, _, _ = run([entry + " ok", "%s/store.hpp:R-F5-NOPE:x reason" % sc.HEADER_DIR])
            self.assertTrue(any("R-F5-ALLOW" in e and "expected" in e for e in errors), errors)
            errors, _, _ = run([entry + " ok", "%s/store.hpp:R-F5-FRIEND:mutation_capability:gone reason"
                                % sc.HEADER_DIR])
            self.assertTrue(any("R-F5-ALLOW: stale entry" in e for e in errors), errors)
            os.remove(os.path.join(inc, "store.hpp"))
            with open(os.path.join(inc, "other.hpp"), "w", encoding="utf-8") as fh:
                fh.write("struct other {};\n")
            errors, _, _ = run([])
            self.assertTrue(any("R-F5-CAP" in e and "not defined" in e for e in errors), errors)
        finally:
            shutil.rmtree(work)


if __name__ == "__main__":
    unittest.main()
