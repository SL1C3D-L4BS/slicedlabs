#!/usr/bin/env bash
# neg-log-schema (WO-030): a log event whose `system` is not a string must make log-schema-check fail and name it.
# Works on a throwaway COPY of the working tree under build/neg/log_schema (no commit needed):
#   1. copy the tree (rsync, build/ and .git left out), run `just log-schema-check` there: the control must exit 0
#   2. append a test case to tests/unit/log_fixture.cpp that writes one otherwise valid event with "system":42 into
#      the SL_LOG_JSON_PATH file, run `just log-schema-check` again: it must exit non-zero, print R-LOG-SCHEMA and
#      name the field (/system)
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/log_schema"
victim="tests/unit/log_fixture.cpp"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
rsync -a --exclude /build --exclude /.git "$root"/ "$work"/

cd "$work"
echo "neg-log-schema: control run (clean copy)"
if ! just log-schema-check >control.log 2>&1; then
  tail -n 20 control.log
  echo "neg-log-schema: FAIL: control log-schema-check must exit 0 on the clean copy"
  exit 1
fi

cat >>"$victim" <<'CPP'

TEST_CASE("planted by neg-log-schema: an event whose system is not a string") {
  const char *path = std::getenv("SL_LOG_JSON_PATH");
  REQUIRE(path != nullptr);
  std::FILE *out = std::fopen(path, "a");
  REQUIRE(out != nullptr);
  std::fputs(R"({"schema":"sl-log-event/1","level":"info","system":42,"message":"planted","fields":{},)"
             R"("correlation":{"id":0,"scope":"process","tick":0},"where":{"file":"planted.cpp","line":1},)"
             R"("ts":"2026-10-17T00:00:00.000Z"})"
             "\n",
             out);
  std::fclose(out);
}
CPP
echo "neg-log-schema: planted run (an event with a numeric system appended by a test in $victim)"
set +e
just log-schema-check >planted.log 2>&1
status=$?
set -e
grep -E 'R-LOG|log-schema-check' planted.log | head -n 5
if [ "$status" -eq 0 ]; then
  echo "neg-log-schema: FAIL: log-schema-check exited 0 with an event whose system is a number"
  exit 1
fi
if ! grep -q 'R-LOG-SCHEMA /system: expected type string' planted.log; then
  echo "neg-log-schema: FAIL: log-schema-check failed (exit $status) without naming R-LOG-SCHEMA and /system"
  exit 1
fi
echo "neg-log-schema: ok (control exit 0, planted exit $status, R-LOG-SCHEMA names /system)"
