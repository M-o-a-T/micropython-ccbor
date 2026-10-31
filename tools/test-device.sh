#!/usr/bin/env bash
# Run the ccbor tests on a board flashed with the test firmware
# (tools/build-esp32.sh BOARD [VARIANT] --test).
#
# Usage: tools/test-device.sh PORT [--quick]
#   1. the run-tests.py suite (tests/mp), the official MicroPython test runner
#   2. the differential test against refcbor (frozen into the test firmware)
#   3. the cbor-test-vectors conformance
#   4. the fuzzer, on the real hardware
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:?usage: $0 PORT [--quick]}"
QUICK=0
[[ "${2:-}" == "--quick" ]] && QUICK=1
PY="$ROOT/.venv/bin/python"
MPREMOTE="$ROOT/.venv/bin/mpremote"
rc=0

step() { echo; echo "== $*"; }

step "run-tests.py suite on $PORT"
results="$ROOT/build/test-results/$(basename "$PORT")"
mkdir -p "$results"
(cd "$ROOT/micropython/tests" && "$PY" run-tests.py -t "port:$PORT" -r "$results" "$ROOT"/tests/mp/*.py) || rc=1

step "differential test against refcbor"
seeds="1 2 3"
[[ $QUICK == 1 ]] && seeds="1"
for seed in $seeds; do
    out="$("$MPREMOTE" connect "$PORT" exec "import sys; sys.argv[:] = ['', 'quick', 'seed=$seed']" \
        run "$ROOT/tests/diff/diff_ccbor.py" | grep -v '^Warning: Comparison' | tail -1)"
    echo "seed $seed: $out"
    [[ "$out" == RESULT:\ OK* ]] || rc=1
done

step "cbor-test-vectors conformance"
# The suite goes inside the script: each mpremote session soft-resets the
# board, and the test firmware then starts with an empty RAM filesystem.
script="$(mktemp -t ccbor-conformance).py"
python3 - "$ROOT/third_party/cbor-test-vectors/tests/rfc8949-appendixA/ (converted; see phase 4)" \
    "$ROOT/tests/conformance/conformance.py" > "$script" <<'PY'
import sys
print("SUITE_JSON = %r" % open(sys.argv[1], encoding="utf-8").read())
print(open(sys.argv[2], encoding="utf-8").read())
PY
out="$("$MPREMOTE" connect "$PORT" run "$script" 2>&1 | tail -1)"
rm -f "$script"
echo "$out"
[[ "$out" == RESULT:\ OK* ]] || rc=1

step "fuzzer"
iterations=5000
[[ $QUICK == 1 ]] && iterations=1000
out="$("$MPREMOTE" connect "$PORT" exec "import sys; sys.argv[:] = ['', 'iterations=$iterations']" \
    run "$ROOT/tests/fuzz/fuzz_ccbor.py" | grep -v '^Warning: Comparison' | tail -1)"
echo "$out"
[[ "$out" == RESULT:\ OK* ]] || rc=1

echo
[[ $rc == 0 ]] && echo "ALL DEVICE TESTS PASSED on $PORT" || echo "SOME DEVICE TESTS FAILED on $PORT"
exit $rc
