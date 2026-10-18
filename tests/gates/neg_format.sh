#!/usr/bin/env bash
# neg-format (WO-001, upgraded by WO-020): a badly formatted C++ file must fail the local CI at the format step.
# Works on a throwaway COPY of the working tree (there may be no commit to clone yet):
#   1. copy the tree (tracked and untracked, ignored files left out) to build/neg/format, git init there
#   2. control run: `just fmt-check` must exit 0 (the full `ci-local.sh --quick` control would build; the
#      end-to-end green control is neg-ci-local-red's)
#   3. plant examples/sandbox/bad_format.cpp, run `bash scripts/ci-local.sh --quick`: must exit non-zero,
#      fail at the fmt-check step (before any build) and name bad_format.cpp
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/format"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

git ls-files -z --cached --others --exclude-standard | tar --null --files-from=- -cf - | tar -xf - -C "$work"

cd "$work"
git init -q
git add -A

echo "neg-format: control run (clean copy)"
if ! just fmt-check; then
  echo "neg-format: FAIL: control run must exit 0 but fmt-check failed on the clean copy"
  exit 1
fi

mkdir -p examples/sandbox
cat >examples/sandbox/bad_format.cpp <<'CPP'
int   main( ){int   x=1;
      if(x){return   x ;}
return 0;}
CPP
git add examples/sandbox/bad_format.cpp

echo "neg-format: planted run (bash scripts/ci-local.sh --quick with examples/sandbox/bad_format.cpp)"
set +e
out="$(bash scripts/ci-local.sh --quick 2>&1)"
status=$?
set -e
printf '%s\n' "$out" | grep -E "FAILED at step|bad_format" | head -n 4

if [ "$status" -eq 0 ]; then
  echo "neg-format: FAIL: ci-local exited 0 on a badly formatted file"
  exit 1
fi
if ! printf '%s\n' "$out" | grep -q "FAILED at step 'fmt-check'"; then
  echo "neg-format: FAIL: ci-local failed (exit $status) but not at the fmt-check step"
  exit 1
fi
if ! printf '%s\n' "$out" | grep -q 'bad_format\.cpp'; then
  echo "neg-format: FAIL: ci-local failed (exit $status) but its output does not name bad_format.cpp"
  exit 1
fi
echo "neg-format: ok (control exit 0, planted ci-local exit $status at fmt-check, bad_format.cpp named)"
