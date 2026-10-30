"""Interoperability with cbor2, the reference implementation of CBOR for
CPython: data packed by one side must unpack on the other, and both must
choose the same (canonical) bytes for the same value.

cbor2 note: the Debian package (6.1.2, the Rust implementation) is what the
tests run against; a stale 5.9.0 dist-info may shadow importlib.metadata,
so nothing here may depend on cbor2.__version__.
"""

import math
import random
import struct

import cbor2
import pytest

from conftest import run_mp

# Records are length-prefixed binary (4 bytes, big endian), not text lines:
# reading very long lines is slow on MicroPython.
SCRIPT = """
import sys
import ccbor
with open(sys.argv[1], "rb") as f, open(sys.argv[2], "wb") as out:
    while True:
        n = f.read(4)
        if not n:
            break
        obj = ccbor.loads(f.read(int.from_bytes(n, "big")))
        data = ccbor.dumps(obj, canonical=True)
        out.write(len(data).to_bytes(4, "big"))
        out.write(data)
"""


def write_records(path, blobs):
    path.write_bytes(b"".join(len(b).to_bytes(4, "big") + b for b in blobs))


def read_records(path):
    data, out, i = path.read_bytes(), [], 0
    while i < len(data):
        n = int.from_bytes(data[i : i + 4], "big")
        out.append(data[i + 4 : i + 4 + n])
        i += 4 + n
    return out


def rand_obj(rng, depth=0, single=False):
    r = rng.randrange(14 if depth < 4 else 8)
    if r == 0:
        return rng.choice([None, True, False, ...])
    if r == 1:
        return rng.randrange(-(2**63), 2**64)
    if r == 2:
        return rng.randrange(-200, 200)
    if r == 3:
        x = rng.uniform(-1e6, 1e6)
        return struct.unpack(">f", struct.pack(">f", x))[0] if single else x
    if r == 4:
        n = rng.choice([0, 1, 31, 32, 255, 256, 300, 70000])
        return "".join(rng.choice("aé€😀z") for _ in range(n))
    if r == 5:
        return rng.randbytes(rng.choice([0, 1, 255, 256, 70000]))
    if r == 6:
        return rng.choice([2**63 - 1, -(2**63), 2**64 - 1, 0, -1, -32, -33, 127, 128])
    if r == 7:
        return float(rng.choice(["inf", "-inf"]))
    if r < 11:
        return [rand_obj(rng, depth + 1, single) for _ in range(rng.choice([0, 1, 15, 16, 17]))]
    return {
        rng.choice([str(rng.random()), rng.randrange(-1000, 1000), rng.randbytes(3)]): rand_obj(
            rng, depth + 1, single
        )
        for _ in range(rng.choice([0, 1, 15, 16, 17]))
    }


def same(a, b):
    if isinstance(a, float) and isinstance(b, float):
        return a == b or (math.isnan(a) and math.isnan(b))
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if b is ...:
        return a is ... or type(a) is type(cbor2.undefined)
    return type(a) is type(b) and a == b


def _raw_tag(tagn):
    def fn(content, shareable):
        return cbor2.CBORTag(tagn, content)

    return fn


# tags 2/3 are the native bignums (kept semantic); the rest stay raw
RAW_TAGS = {n: _raw_tag(n) for n in range(256) if n not in (2, 3)}


def to_c2(o):
    if o is ...:
        return cbor2.undefined
    if isinstance(o, list):
        return [to_c2(x) for x in o]
    if isinstance(o, dict):
        return {to_c2(k): to_c2(v) for k, v in o.items()}
    return o


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_cbor2_round_trip(bigint_flavour, quick, seed, tmp_path):
    flavour = bigint_flavour
    """CPython packs, ccbor unpacks and packs again (canonical), CPython unpacks."""
    single = flavour == "float32"
    rng = random.Random(seed)
    objs = [rand_obj(rng, single=single) for _ in range(200 if quick else 2000)]
    src = tmp_path / "in.bin"
    dst = tmp_path / "out.bin"
    write_records(src, [cbor2.dumps(to_c2(o), canonical=True) for o in objs])
    script = tmp_path / "interop.py"
    script.write_text(SCRIPT)
    run_mp(flavour, script, src, dst)
    back = read_records(dst)
    assert len(back) == len(objs)
    for obj, data in zip(objs, back):
        assert same(cbor2.loads(data, semantic_decoders=RAW_TAGS), obj), obj


def _f32_exact(v):
    """True if the double survives single-precision rounding exactly."""
    try:
        back = struct.unpack(">f", struct.pack(">f", v))[0]
        return struct.pack(">d", back) == struct.pack(">d", v)
    except OverflowError:
        return False


def test_same_bytes_as_cbor2(bigint_flavour, tmp_path):
    flavour = bigint_flavour
    """For the same value both choose the same (canonical) encoding."""
    # Python expressions, evaluated on both sides (literals this big do not
    # compile on MicroPython).
    exprs = ["None", "True", "False", "0", "127", "128", "255", "256", "65535", "65536",
             "2**32 - 1", "2**32", "2**64 - 1", "2**64", "-1", "-32", "-33", "-128", "-129",
             "-32768", "-32769", "-(2**31)", "-(2**31) - 1", "-(2**63)", "-(2**63) - 1",
             "-(2**64)", "-(2**64) - 1", "2**100", "-(2**100)", "''", "'a' * 31", "'a' * 32",
             "'a' * 255", "'a' * 256", "'a' * 65535", "'a' * 65536", "b''", "b'x' * 255",
             "b'x' * 256", "b'x' * 65536", "[]", "[0] * 15", "[0] * 16", "[0] * 65536", "{}",
             "{i: i for i in range(16)}"]  # fmt: skip
    script = tmp_path / "same.py"
    script.write_text(
        "import ccbor\nfrom binascii import hexlify\n"
        f"for e in {exprs!r}:\n    print(hexlify(ccbor.dumps(eval(e), canonical=True)).decode())\n"
    )
    values = [eval(e) for e in exprs]
    out = run_mp(flavour, script).split()
    expected = [cbor2.dumps(to_c2(v), canonical=True).hex() for v in values]
    assert out == expected
    # floats: smallest exact width, like cbor2 canonical
    single = flavour == "float32"
    script.write_text(
        "import ccbor\nfrom binascii import hexlify\n"
        "for v in (0.5, -2.25, 1e10, float('inf'), float('nan'), 1.5, 65504.0, 0.0, -0.0):\n"
        "    print(hexlify(ccbor.dumps(v, canonical=True)).decode())\n"
    )
    out = run_mp(flavour, script).split()
    floats = [0.5, -2.25, 1e10, float("inf"), float("nan"), 1.5, 65504.0, 0.0, -0.0]
    expected = []
    for v in floats:
        if single and not _f32_exact(v):
            continue  # a float32 build cannot hold this double exactly
        expected.append(cbor2.dumps(v, canonical=True).hex())
    assert out == expected


def test_tag_interop(bigint_flavour, tmp_path):
    flavour = bigint_flavour
    """CBORTag <-> Tag, and undefined <-> Ellipsis."""
    script = tmp_path / "tag.py"
    script.write_text(
        "import ccbor\nfrom binascii import hexlify\n"
        "t = ccbor.Tag(32, 'a')\n"
        "print(hexlify(ccbor.dumps(t)).decode())\n"
        "v = ccbor.loads(bytes.fromhex('d8206161'))\n"
        "print(type(v).__name__, v.tag, v.value)\n"
        "print(hexlify(ccbor.dumps(...)).decode())\n"
        "v = ccbor.loads(bytes.fromhex('f7'))\n"
        "print(v is Ellipsis)\n"
    )
    out = run_mp(flavour, script).splitlines()
    assert out[0] == cbor2.dumps(cbor2.CBORTag(32, "a")).hex()
    assert out[1] == "Tag 32 a"
    assert out[2] == "f7"
    assert out[3] == "True"
