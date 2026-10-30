"""The refcbor oracle gate: refcbor (pure Python) against cbor2 on the host.

refcbor is the differential test's reference implementation; before it can
be trusted as an oracle it must agree with cbor2, the reference CBOR
library for CPython: byte-identical canonical packing and equal
round-trips. Runs on CPython (not on MicroPython).

Also checks the RFC 8949 appendix A vectors.
"""

import math
import random
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "diff"))

import cbor2  # noqa: E402

import refcbor  # noqa: E402


def _eq(a, b):
    if isinstance(a, float) and isinstance(b, float):
        if math.isnan(a) and math.isnan(b):
            return True
        return struct.pack(">d", a) == struct.pack(">d", b)
    if isinstance(b, (list, tuple)):
        return (
            isinstance(a, (list, tuple))
            and len(a) == len(b)
            and all(_eq(x, y) for x, y in zip(a, b))
        )
    if isinstance(b, dict):
        return (
            isinstance(a, dict)
            and len(a) == len(b)
            and all(
                any(_eq(ka, kb) and _eq(va, vb) for ka, va in a.items()) for kb, vb in b.items()
            )
        )
    if isinstance(b, refcbor.Tag):
        return isinstance(a, refcbor.Tag) and a.tag == b.tag and _eq(a.value, b.value)
    return type(a) is type(b) and a == b


def _eq_c2(a, b):
    if isinstance(a, float) and isinstance(b, float):
        if math.isnan(a) and math.isnan(b):
            return True
        return struct.pack(">d", a) == struct.pack(">d", b)
    if isinstance(b, (list, tuple)):
        return (
            isinstance(a, (list, tuple))
            and len(a) == len(b)
            and all(_eq_c2(x, y) for x, y in zip(a, b))
        )
    if hasattr(b, "items") and not isinstance(b, (str, bytes)):
        # a dict (ours) or cbor2's frozendict (tag contents decode immutably)
        return (
            hasattr(a, "items")
            and len(a) == len(b)
            and all(
                any(_eq_c2(ka, kb) and _eq_c2(va, vb) for ka, va in a.items())
                for kb, vb in b.items()
            )
        )
    if isinstance(b, refcbor.Tag):
        return type(a).__name__ == "CBORTag" and a.tag == b.tag and _eq_c2(a.value, b.value)
    if b is ...:
        return type(a) is type(cbor2.undefined)
    return type(a) is type(b) and a == b


def robj(depth=0):
    if depth > 3:
        return random.choice([0, 1, -1, "s", b"b", 2.5, True, None, 23, 24])
    r = random.getrandbits(4)
    if r == 0:
        return random.choice([None, True, False, ...])
    if r == 1:
        return random.choice(
            [
                0,
                1,
                23,
                24,
                255,
                256,
                65535,
                65536,
                2**32 - 1,
                2**32,
                2**64 - 1,
                -1,
                -24,
                -25,
                -256,
                -257,
                -65536,
                -65537,
                -(2**32),
                -(2**32) - 1,
                2**64,
                -(2**64) - 1,
                2**100,
                -(2**100),
            ]
        )
    if r == 2:
        return struct.unpack(">d", struct.pack(">Q", random.getrandbits(64)))[0]
    if r == 3:
        return random.choice(
            [0.0, -0.0, 0.5, 1.5, 2.5, -4.0, 65504.0, 1e10, 1.5e-8, 3.141592653589793, 1e300]
        )
    if r == 4:
        return "".join(
            random.choice("abe") for _ in range(random.choice([0, 1, 23, 24, 255, 300]))
        )
    if r == 5:
        return bytes(random.getrandbits(8) for _ in range(random.choice([0, 1, 23, 24, 255])))
    if r == 6:
        # tags 2/3 are the native bignums: content must be a byte string,
        # which the generator cannot produce meaningfully
        return refcbor.Tag(random.choice([0, 1, 4, 5, 30, 32, 100, 55798]), robj(depth + 1))
    if r < 11:
        return [robj(depth + 1) for _ in range(random.choice([0, 1, 2, 5, 23]))]
    keys = []
    while len(keys) < random.choice([1, 2, 3]):
        k = random.choice(
            ["k", "zz", "", "a" * 300, random.randrange(-1000, 1000), 2**64, True, 0.5]
        )
        if k not in keys:
            keys.append(k)
    return {k: robj(depth + 1) for k in keys}


def test_appendix_a_vectors():
    from binascii import unhexlify

    vectors = [
        (0, "00"),
        (1, "01"),
        (10, "0a"),
        (23, "17"),
        (24, "1818"),
        (25, "1819"),
        (100, "1864"),
        (1000, "1903e8"),
        (1000000, "1a000f4240"),
        (1000000000000, "1b000000e8d4a51000"),
        (18446744073709551615, "1bffffffffffffffff"),
        (18446744073709551616, "c249010000000000000000"),
        (-18446744073709551616, "3bffffffffffffffff"),
        (-18446744073709551617, "c349010000000000000000"),
        (-1, "20"),
        (-10, "29"),
        (-100, "3863"),
        (-1000, "3903e7"),
        (-1000000, "3a000f423f"),
        (-1000000000000, "3b000000e8d4a50fff"),
        (0.0, "f90000"),
        (1.0, "f93c00"),
        (1.5, "f93e00"),
        (65504.0, "f97bff"),
        (1e10, "fa501502f9"),
        (3.4028234663852886e38, "fa7f7fffff"),  # FLT_MAX: exact in single
        (1.0e300, "fb7e37e43c8800759c"),
        (3.141592653589793, "fb400921fb54442d18"),
        ("", "60"),
        ("a", "6161"),
        ("IETF", "6449455446"),
        ("\u00fc", "62c3bc"),
        (b"", "40"),
        (b"\x01\x02\x03\x04", "4401020304"),
        ([], "80"),
        ([1, 2, 3], "83010203"),
        ([1, [2, 3], [4, 5]], "8301820203820405"),
        ({}, "a0"),
        ({1: 2, 3: 4}, "a201020304"),
        ({"a": 1, "b": [2, 3]}, "a26161016162820203"),
        (["a", {"b": "c"}], "826161a161626163"),
        (
            {"a": "A", "b": "B", "c": "C", "d": "D", "e": "E"},
            "a56161614161626142616361436164614461656145",
        ),
        (False, "f4"),
        (True, "f5"),
        (None, "f6"),
    ]
    for value, hexstr in vectors:
        data = unhexlify(hexstr)
        assert refcbor.dumps(value, canonical=True) == data, (value, hexstr)
        assert _eq(refcbor.loads(data), value), (value, hexstr)
        # cbor2 agrees on the decode (Ellipsis/undefined aside)
        if value is not ...:
            assert _eq_c2(cbor2.loads(data), value), (value, hexstr)


def _raw_tag(tagn):
    def fn(content, shareable):
        return cbor2.CBORTag(tagn, content)

    return fn


# cbor2 converts many tag numbers semantically (dates, fractions, ...):
# keep them all raw, like the conformance converter does.
# tags 2 and 3 are the native bignum tags: keep cbor2's int conversion
_RAW_TAGS = {n: _raw_tag(n) for n in range(256) if n not in (2, 3)}


def _to_c2(o):
    if isinstance(o, refcbor.Tag):
        return cbor2.CBORTag(o.tag, _to_c2(o.value))
    if isinstance(o, (list, tuple)):
        return [_to_c2(x) for x in o]
    if isinstance(o, dict):
        return {_to_c2(k): _to_c2(v) for k, v in o.items()}
    if o is ...:
        return cbor2.undefined
    return o


def test_random_objects():
    random.seed(42)
    for _ in range(2000):
        o = robj()
        mine = refcbor.dumps(o, canonical=True)
        theirs = cbor2.dumps(_to_c2(o), canonical=True)
        assert mine == theirs, (repr(o)[:80], mine.hex()[:60], theirs.hex()[:60])
        back = refcbor.loads(mine)
        assert _eq(back, o), (repr(o)[:80], repr(back)[:80])
        assert _eq_c2(cbor2.loads(mine, semantic_decoders=_RAW_TAGS), o), repr(o)[:80]


def test_decode_widths():
    from binascii import unhexlify

    # non-minimal and indefinite encodings decode correctly
    assert refcbor.loads(unhexlify("1800")) == 0
    assert refcbor.loads(unhexlify("190001")) == 1
    assert refcbor.loads(unhexlify("9f0102ff")) == [1, 2]
    assert refcbor.loads(unhexlify("bf61610161629f0203ffff")) == {"a": 1, "b": [2, 3]}
    assert refcbor.loads(unhexlify("5f42010243030405ff")) == b"\x01\x02\x03\x04\x05"
    assert refcbor.loads(unhexlify("7f657374726561646d696e67ff")) == "streaming"
    assert refcbor.loads(unhexlify("c249010000000000000000")) == 2**64
    assert refcbor.loads(unhexlify("c349010000000000000000")) == -(2**64) - 1
    assert refcbor.loads(unhexlify("d9d9f701")) == 1  # 55799 stripped
    # errors
    for bad in ("1c", "f818", "ff", "1f", "3f", "5f01ff", "bf61 61 ff".replace(" ", "")):
        try:
            refcbor.loads(unhexlify(bad))
            raise AssertionError(bad)
        except refcbor.CCBORException:
            pass
        except RuntimeError:
            pass  # depth
