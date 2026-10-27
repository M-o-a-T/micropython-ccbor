#!/usr/bin/env bash
# Line and branch coverage of cmodules/ccbor/modccbor.c: build the
# coverage variant of the unix port, run every test program on it, report.
#
# Usage: tools/coverage.sh        (report in build/unix-coverage/ccbor/*.gcov)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$ROOT/build/unix-coverage"
"$ROOT/tools/build-unix.sh" coverage >/dev/null
find "$B" -name "*.gcda" -delete

CCBOR_FLAVOURS=coverage "$ROOT/.venv/bin/python" -m pytest -q "$ROOT/tests/host/test_suites.py"

cd "$B/ccbor"
if command -v xcrun >/dev/null; then gcov=(xcrun llvm-cov gcov); else gcov=(gcov); fi
"${gcov[@]}" -b modccbor.gcda | grep -A4 "modccbor.c'"
echo "Not executed (#####): $(grep -c '#####' modccbor.c.gcov) lines; see $B/ccbor/modccbor.c.gcov"
