"""The MicroPython-side test programs, run on each unix build:
the run-tests.py suite, the differential test, the fuzzer and the
conformance check. (Differential/conformance move from umsgpack and
msgpack-test-suite to refcbor and cbor-test-vectors in phases 4/5.)"""

import os

import pytest
import subprocess

from conftest import INTBIG_TESTS, LONGLONG, ROOT, micropython, run_mp

PETER = ROOT / "third_party" / "micropython-msgpack"


def test_run_tests_suite(flavour):
    tests = sorted(
        str(p)
        for p in (ROOT / "tests" / "mp").glob("*.py")
        if flavour not in LONGLONG or p.name not in INTBIG_TESTS
    )
    env = dict(os.environ, MICROPY_MICROPYTHON=str(micropython(flavour)))
    proc = subprocess.run(
        ["python3", "run-tests.py", "--heapsize", "32M", *tests],
        cwd=ROOT / "micropython" / "tests",
        capture_output=True,
        text=True,
        env=env,
    )
    assert proc.returncode == 0, proc.stdout[-4000:] + proc.stderr[-2000:]


@pytest.mark.skip(reason="phase 5: differential moves to refcbor")
def test_differential(bigint_flavour, quick):
    out = run_mp(bigint_flavour, ROOT / "tests/diff/diff_ccbor.py", *(["quick"] if quick else []),
                 path=[PETER])  # fmt: skip
    assert "RESULT: OK" in out, out[-3000:]


@pytest.mark.skip(reason="phase 5: fuzzer updated for CBOR (needs loads)")
def test_fuzz(bigint_flavour, quick):
    out = run_mp(bigint_flavour, ROOT / "tests/fuzz/fuzz_ccbor.py",
                 f"iterations={5000 if quick else 100000}", heap="128M")  # fmt: skip
    assert "RESULT: OK" in out, out[-3000:]


def test_conformance(bigint_flavour):
    suite = PETER.parent / "msgpack-test-suite" / "dist" / "msgpack-test-suite.json"
    if not suite.exists():
        pytest.skip("msgpack-test-suite submodule not fetched (replaced in phase 4)")
    out = run_mp(bigint_flavour, ROOT / "tests/conformance/conformance.py", suite)
    assert "RESULT: OK" in out, out
