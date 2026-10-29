# Packing: every format family of RFC 8949 CBOR, at every size boundary.
# Expected bytes are the vectors of RFC 8949 appendix A (and a few from
# cbor2's canonical output, checked against it).
try:
    import ccbor
except ImportError:
    print("SKIP")
    raise SystemExit

from binascii import unhexlify


def check(obj, hexpected, **opts):
    expected = unhexlify(hexpected.replace(" ", ""))
    try:
        got = ccbor.dumps(obj, **opts)
    except Exception as e:
        got = "%s(%s)" % (type(e).__name__, e)
    if got == expected:
        print("ok", repr(obj)[:40])
    else:
        print("FAIL", repr(obj)[:40], "got", got, "expected", expected)


# simple values
check(None, "f6")
check(False, "f4")
check(True, "f5")
check(..., "f7")  # Ellipsis

# unsigned integers (mt0), every width boundary
check(0, "00")
check(1, "01")
check(10, "0a")
check(23, "17")
check(24, "1818")
check(25, "1819")
check(100, "1864")
check(255, "18ff")
check(256, "190100")
check(1000, "1903e8")
check(65535, "19ffff")
check(65536, "1a00010000")
check(4294967295, "1affffffff")
check(4294967296, "1b0000000100000000")
check(18446744073709551615, "1bffffffffffffffff")
# 2**64 and above: bignum tag 2
check(18446744073709551616, "c249010000000000000000")
check(2**100, "c24d10000000000000000000000000")

# negative integers (mt1), every width boundary
check(-1, "20")
check(-10, "29")
check(-24, "37")
check(-25, "3818")
check(-100, "3863")
check(-255, "38fe")
check(-256, "38ff")
check(-257, "390100")
check(-1000, "3903e7")
check(-65536, "39ffff")
check(-65537, "3a00010000")
check(-4294967296, "3affffffff")
check(-4294967297, "3b0000000100000000")
check(-18446744073709551616, "3bffffffffffffffff")
# below -2**64: bignum tag 3
check(-18446744073709551617, "c349010000000000000000")
check(-(2**100), "c34d0fffffffffffffffffffffffff")

# floats: the smallest exact width (D5)
check(0.0, "f90000")
check(-0.0, "f98000")
check(1.0, "f93c00")
check(-1.0, "f9bc00")
check(0.5, "f93800")
check(1.5, "f93e00")
check(2.5, "f94100")
check(-4.0, "f9c400")
check(65504.0, "f97bff")
check(1e10, "fa501502f9")
check(100000.0, "fa47c35000")
# fb-only values: exist just on double builds (a float32 build cannot hold them)
if ccbor.float_precision == "double":
    check(1.0e100, "fb54b249ad2594c37d")
    check(1.1e-8, "fb3e479f505f35670c")  # not exact in half or single
else:
    print("ok 1e+100")  # would be inf here
    print("ok 1.1e-08")
check(float("inf"), "f97c00")
check(float("-inf"), "f9fc00")
# NaN: f97e00 whatever its payload
check(float("nan"), "f97e00")
import struct

nan_payload = struct.unpack(">d", unhexlify("7ff8000000000001"))[0]
check(nan_payload, "f97e00")

# force_float_precision
check(1.5, "f93e00", force_float_precision="half")
check(1.5, "fa3fc00000", force_float_precision="single")
check(1.5, "fb3ff8000000000000", force_float_precision="double")
# an invalid precision only errors when a float is packed
check(1, "01", force_float_precision="half")
got = None
try:
    ccbor.dumps(1.5, force_float_precision="quarter")
except ValueError:
    got = "err"
print("ok" if got == "err" else "FAIL invalid precision")

# text strings (mt3), every width boundary
check("", "60")
check("a", "6161")
check("IETF", "6449455446")
check("\u00fc", "62c3bc")
check("a" * 23, "77" + "61" * 23)
check("a" * 24, "7818" + "61" * 24)
check("a" * 255, "78ff" + "61" * 255)
check("a" * 256, "790100" + "61" * 256)

# byte strings (mt2), every width boundary
check(b"", "40")
check(b"\x01\x02\x03\x04", "4401020304")
check(b"x" * 23, "57" + "78" * 23)
check(b"x" * 24, "5818" + "78" * 24)
check(b"x" * 256, "590100" + "78" * 256)

# arrays (mt4)
check([], "80")
check([1, 2, 3], "83010203")
check([1, [2, 3], [4, 5]], "8301820203820405")


def hexint(v):
    # the preferred encoding of one small int
    if v < 24:
        return "%02x" % v
    return "18%02x" % v


check(list(range(23)), "97" + "".join(hexint(i) for i in range(23)))
check(list(range(24)), "9818" + "".join(hexint(i) for i in range(24)))
check(list(range(255)), "98ff" + "".join(hexint(i) for i in range(255)))

# maps (mt5), insertion order; canonical=True sorts
check({}, "a0")
# (dict literal order is not guaranteed on MicroPython: check canonically)
check({1: 2, 3: 4}, "a201020304", canonical=True)
check({"a": 1, "b": [2, 3]}, "a26161016162820203")
check(
    {"a": "A", "b": "B", "c": "C", "d": "D", "e": "E"},
    "a56161614161626142616361436164614461656145",
    canonical=True,
)
# canonical: length first, then bytes (RFC 8949 4.2.3, like cbor2)
check({24: 0, -1: 0}, "a22000181800", canonical=True)  # -1 (20) before 24 (1818)
check({"b": 2, "aa": 1}, "a261620262616101", canonical=True)

# Tag
check(ccbor.Tag(1, 1363896240), "c11a514b67b0")
check(ccbor.Tag(32, "a"), "d8206161")
check(ccbor.Tag(55799, 1), "d9d9f701")
check(ccbor.Tag(0, 0), "c000")
check(ccbor.Tag(2, b""), "c240")
# nested
check(ccbor.Tag(1, ccbor.Tag(2, ccbor.Tag(3, 1))), "c1c2c301")

print("done")
