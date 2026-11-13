#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

T="$SRC/build/tests"
passed=0 failed=0

# expect_line <name> <fixed-string-regex> <file>: the test's own summary line must be present
expect() {
  local name="$1" pat="$2" f="$3"
  if grep -Eq -- "$pat" "$f"; then passed=$((passed+1)); echo "PASS $name"
  else failed=$((failed+1)); echo "FAIL $name (no line matching: $pat)"; fi
}

for t in test-link test-uartrx test-midi-cin kat; do
  [ -x "$T/$t" ] || { echo "missing $T/$t - mayhem/build.sh did not build it" >&2; emit_ctrf "guitarpedal" 0 1 0; exit 1; }
done

out=$(mktemp -d)
"$T/test-link"     >"$out/link"   2>&1; rc_link=$?
"$T/test-uartrx"   >"$out/uartrx" 2>&1; rc_uartrx=$?
"$T/test-midi-cin" >"$out/cin"    2>&1; rc_cin=$?
"$T/kat"           >"$out/kat"    2>&1; rc_kat=$?

# The upstream suites print their verdict; a binary that exits 0 without printing it fails here.
[ $rc_link   -eq 0 ] && expect "test-link"     '^test-link: [0-9]+ packets and the flow control, all as expected$' "$out/link" || { failed=$((failed+1)); echo "FAIL test-link rc=$rc_link"; }
[ $rc_uartrx -eq 0 ] && expect "test-uartrx"   '^test-uartrx: [0-9]+ END races retried, [0-9]+ zeros on the last byte, all as expected$' "$out/uartrx" || { failed=$((failed+1)); echo "FAIL test-uartrx rc=$rc_uartrx"; }
[ $rc_cin    -eq 0 ] && expect "test-midi-cin" '^all CIN checks pass$' "$out/cin" || { failed=$((failed+1)); echo "FAIL test-midi-cin rc=$rc_cin"; }
! grep -q '^FAIL' "$out/link" "$out/uartrx" "$out/cin" && passed=$((passed+1)) || { failed=$((failed+1)); echo "FAIL a suite reported FAIL"; }

# Known-answer probes: exact bytes, worked out by hand (COBS + CRC-8/0x07 independent of link.h).
[ $rc_kat -eq 0 ] || { failed=$((failed+1)); echo "FAIL kat rc=$rc_kat"; }
while IFS= read -r want; do
  if grep -Fxq -- "$want" "$out/kat"; then passed=$((passed+1)); echo "PASS $want"
  else failed=$((failed+1)); echo "FAIL expected: $want"; fi
done <<'KAT'
KAT link_encode midi: 01 01 01 06 03 90 3c 64 ad 00
KAT link_encode zeros: 04 03 01 05 01 03 01 02 03 ff 01 00
KAT link_rx: 03 01 05 00 00 01 02 00 ff
KAT link_crc: dropped 1
KAT midi 1: 09 90 3c 64
KAT midi 2: 09 90 3d 65
KAT midi 3: 0c c1 07
KAT midi packets: 3
KAT
! grep -q 'accepted' "$out/kat" && passed=$((passed+1)) || { failed=$((failed+1)); echo "FAIL corrupt packet accepted"; }

rm -rf "$out"
emit_ctrf "guitarpedal" "$passed" "$failed" 0
