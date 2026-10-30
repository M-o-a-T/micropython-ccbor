"""The MicroPython-side test programs, run on each unix build:
the run-tests.py suite, the differential test, the fuzzer and the
cbor-test-vectors conformance check. (The differential test is skipped
until phase 5 provides the in-repo refcbor reference.)"""

import os

import pytest
import subprocess

from conftest import INTBIG_TESTS, LONGLONG, ROOT, micropython, run_mp


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


def test_differential(bigint_flavour, quick):
    """ccbor (C) against refcbor (pure Python): identical bytes or identical
    exception on pack; equal objects or identical exception on unpack."""
    out = run_mp(bigint_flavour, ROOT / "tests/diff/diff_ccbor.py",
                 *(["quick"] if quick else []), path=[ROOT / "tests" / "diff"])  # fmt: skip
    assert "RESULT: OK" in out, out[-3000:]


def test_fuzz(bigint_flavour, quick):
    out = run_mp(bigint_flavour, ROOT / "tests/fuzz/fuzz_ccbor.py",
                 f"iterations={5000 if quick else 50000}", heap="128M")  # fmt: skip
    assert "RESULT: OK" in out, out[-3000:]


def test_conformance(bigint_flavour, tmp_path):
    """The cbor-test-vectors conformance, read from the .cbor sidecars and
    converted to JSON on the host (edn2json.py); the MicroPython driver
    checks every case and reports the exact skip count."""
    vectors = ROOT / "third_party" / "cbor-test-vectors" / "tests"
    if not vectors.exists():
        pytest.skip("cbor-test-vectors submodule not fetched")
    out = subprocess.run(
        ["python3", str(ROOT / "tests" / "conformance" / "edn2json.py")],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    json_path = tmp_path / "vectors.json"
    json_path.write_text(out)
    res = run_mp(bigint_flavour, ROOT / "tests/conformance/conformance.py", json_path, heap="256M")
    assert "RESULT: OK" in res, res[-3000:]
