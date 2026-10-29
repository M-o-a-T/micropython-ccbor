# Unpacking: every format of RFC 8949 CBOR (including encodings ccbor never
# produces, such as non-minimal ints and indefinite lengths), the load
# options and the input types.
try:
    import ccbor
except ImportError:
    print("SKIP")
    raise SystemExit

from binascii import unhexlify


def check(hexdata, expected, **opts):
    data = unhexlify(hexdata.replace(" ", ""))
    try:
        got = ccbor.loads(data, **opts)
    except Exception as e:
        got = "%s(%s)" % (type(e).__name__, e)
    if type(got) is type(expected) and got == expected:
        print("ok", hexdata[:24])
    else:
        print("FAIL", hexdata[:24], "got", repr(got)[:60], "expected", repr(expected)[:60])


# simple values
check("f6", None)
check("f4", False)
check("f5", True)
check("f7", ...)

# unsigned integers, all widths, also the non-minimal ones
check("00", 0)
check("0a", 10)
check("17", 23)
check("1818", 24)
check("1864", 100)
check("1800", 0)  # non-minimal
check("190100", 256)
check("190001", 1)  # non-minimal
check("1a00010000", 65536)
check("1affffffff", 4294967295)
check("1bffffffffffffffff", 18446744073709551615)
check("1b0000000000000001", 1)  # non-minimal

# negative integers, all widths
check("20", -1)
check("29", -10)
check("37", -24)
check("3818", -25)
check("3863", -100)
check("3903e7", -1000)
check("39ffff", -65536)
check("3a00010000", -65537)
check("3affffffff", -4294967296)
check("3bffffffffffffffff", -18446744073709551616)
check("3b0000000100000000", -4294967297)  # non-minimal

# bignum tags 2/3
check("c249010000000000000000", 2**64)
check("c349010000000000000000", -(2**64) - 1)
check("c240", 0)
check("c340", -1)
check("c2420005", 5)  # leading zeros accepted

# floats: half, single, double
check("f90000", 0.0)
check("f98000", -0.0)
check("f93c00", 1.0)
check("f93e00", 1.5)
check("f94100", 2.5)
check("f97bff", 65504.0)
check("f97c00", float("inf"))
check("f9fc00", float("-inf"))
check("fa47c35000", 100000.0)
check("fa501502f9", 1e10)
# a double-precision pi: exact only on double builds
if ccbor.float_precision == "double":
    check("fb400921fb54442d18", 3.141592653589793)
else:
    check("fb400921fb54442d18", 3.1415927)
nan = ccbor.loads(b"\xf9\x7e\x00")
print("nan:", nan != nan)
nan2 = ccbor.loads(b"\xfb\x7f\xf8\x00\x00\x00\x00\x00\x01")  # NaN with payload
print("nan payload:", nan2 != nan2)

# text strings, all widths
check("60", "")
check("6161", "a")
check("6449455446", "IETF")
check("62c3bc", "\u00fc")
check("77" + "61" * 23, "a" * 23)
check("7818" + "61" * 24, "a" * 24)
check("7f657374726561646d696e67ff", "streaming")  # indefinite

# byte strings, all widths
check("40", b"")
check("4401020304", b"\x01\x02\x03\x04")
check("57" + "78" * 23, b"x" * 23)
check("5818" + "78" * 24, b"x" * 24)
check("5f42010243030405ff", b"\x01\x02\x03\x04\x05")  # indefinite

# arrays, all widths
check("80", [])
check("83010203", [1, 2, 3])
check("9f0102ff", [1, 2])  # indefinite
check("9f018202039f0405ffff", [1, [2, 3], [4, 5]])
check("97" + "".join("%02x" % i for i in range(23)), list(range(23)))
check("9818" + "".join("18%02x" % i for i in range(24)), list(range(24)))

# maps, all widths
check("a0", {})
check("a201020304", {1: 2, 3: 4})
check("a26161016162820203", {"a": 1, "b": [2, 3]})
check("bf61610161629f0203ffff", {"a": 1, "b": [2, 3]})  # indefinite
check(
    "a56161614161626142616361436164614461656145",
    {"a": "A", "b": "B", "c": "C", "d": "D", "e": "E"},
)

# tags
check("c11a514b67b0", ccbor.Tag(1, 1363896240))
check("d8206161", ccbor.Tag(32, "a"))
check("d9d9f701", 1)  # 55799 self-describe: stripped
check("81d9d9f701", [1])  # also nested
check("c1c4c501", ccbor.Tag(1, ccbor.Tag(4, ccbor.Tag(5, 1))))
# a tag with an unregistered number decodes to Tag


# options
check("8201820102", (1, (1, 2)), use_tuple=True)
check("8201820102", [1, [1, 2]])
check("62fffe", b"\xff\xfe", allow_invalid_utf8=True)
try:
    from collections import OrderedDict

    od = ccbor.loads(b"\xa2\x61c\x01\x61a\x02", use_ordered_dict=True)
    print(type(od).__name__, list(od.keys()))
except ImportError:
    print("OrderedDict ['c', 'a']")

# a list used as a map key becomes a (nested) tuple
print(ccbor.loads(b"\xa1\x82\x01\x81\x02\x61v"))

# the input may be bytes, bytearray or memoryview; trailing data is ignored
print(ccbor.loads(bytearray(b"\x82\x01\x02")))
print(ccbor.loads(memoryview(b"\x01\x82\x01\x02")[1:]))
print(ccbor.loads(b"\x01\x02\x03"))

# map keys that already exist as qstrs are shared, not copied
k1 = list(ccbor.loads(b"\xa1\x64keys\x01"))[0]
k2 = list(ccbor.loads(b"\xa1\x64keys\x02"))[0]
print(k1 == "keys", k1 is k2)

# round trip of a typical telemetry record
rec = {
    "ts": 1729243200,
    "id": "sensor-01",
    "t": -12.5,
    "ok": True,
    "raw": b"\x01\x02",
    "v": [1, 2],
}
print(ccbor.loads(ccbor.dumps(rec)) == rec)
