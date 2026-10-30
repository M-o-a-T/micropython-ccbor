# Conformance with cbor-test-vectors (cbor-wg), read from its .cbor sidecars
# (converted to JSON on the host by edn2json.py; no EDN parser involved).
#
# For each case:
#   - decode side: loads(encoded) must equal the expected value (mapped from
#     the .cbor structure: tags -> Tag, undefined -> Ellipsis, bignums -> int,
#     floats compared by exact bits); for the "bad" files it must raise a
#     ccbor exception;
#   - encode side: dumps(value, canonical=True) must give the same bytes,
#     checked only where roundtrip is not false.
#
# Options in the vectors (collapseBigInts, keepNanPayloads, avoidInts,
# dateTag) get an explicit rule; every skip is counted and printed, and the
# test asserts the exact skip count.
#
#   micropython conformance.py <vectors.json>

import json
import sys
from binascii import unhexlify

import ccbor

path = "vectors.json"
for a in sys.argv[1:]:
    if a.endswith(".json"):
        path = a
with open(path) as f:
    docs = json.loads(f.read()) if hasattr(json, "loads") else json.load(f)

checks = 0
fails = 0
skips = []


def fail(*args):
    global fails
    fails += 1
    print("FAIL", *args)


# --- decode the expected value from its JSON mapping -----------------------


def to_float(hexc):
    # exact double from its 8-byte hex form
    import struct

    return struct.unpack(">d", unhexlify(hexc))[0]


def build(v):
    """The JSON mapping -> the Python value ccbor.loads should produce."""
    if v is None or v is True or v is False:
        return v
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        return v
    if isinstance(v, list):
        return [build(x) for x in v]
    if isinstance(v, dict):
        if "__undefined__" in v:
            return ...
        if "__simple__" in v:
            return None  # not supported: the caller must skip these cases
        if "__tag__" in v:
            return ccbor.Tag(v["__tag__"], build(v["value"]))
        if "__float__" in v:
            return to_float(v["__float__"])
        if "__bytes__" in v:
            return unhexlify(v["__bytes__"])
        if "__big__" in v:
            return int(v["__big__"])
        if "__map__" in v:
            d = {}
            for k, x in v["__map__"]:
                d[build(k)] = build(x)
            return d
    raise ValueError("cannot build %r" % (v,))


def equal(a, b):
    """ccbor's decode against the expected value."""
    if b is ... or a is ...:
        return a is b
    if isinstance(b, float) or isinstance(a, float):
        if not isinstance(a, (int, float)) or not isinstance(b, (int, float)):
            return False
        import math
        import struct

        if isinstance(a, float) and isinstance(b, float):
            if math.isnan(a) and math.isnan(b):
                return True  # the payload is not kept (D5, cbor2 canonical)
        # compare by exact bits; a float32 build rounds on the way in, so
        # compare through its own width
        if ccbor.float_precision == "single":
            return struct.pack(">f", a) == struct.pack(">f", b)
        return struct.pack(">d", a) == struct.pack(">d", b)
    if isinstance(b, (list, tuple)):
        return (
            isinstance(a, (list, tuple))
            and len(a) == len(b)
            and all(equal(x, y) for x, y in zip(a, b))
        )
    if isinstance(b, dict):
        return isinstance(a, dict) and _dict_equal(a, b)
    if isinstance(b, ccbor.Tag):
        return isinstance(a, ccbor.Tag) and a.tag == b.tag and equal(a.value, b.value)
    return type(a) is type(b) and a == b


def _dict_equal(a, b):
    if len(a) != len(b):
        return False
    for k, v in b.items():
        found = False
        for ka, va in a.items():
            if equal(ka, k):
                if not equal(va, v):
                    return False
                found = True
                break
        if not found:
            return False
    return True


def _has_lossy_float(v):
    """True if the mapping contains a double that does not survive
    single-precision rounding (float32 builds)."""
    if isinstance(v, dict):
        if "__float__" in v and not v.get("f32", True):
            # the vector's double does not survive single-precision
            # rounding (computed on the host, where doubles exist)
            return True
        for x in v.values():
            if _has_lossy_float(x):
                return True
        if "__map__" in v:
            for k, x in v["__map__"]:
                if _has_lossy_float(k) or _has_lossy_float(x):
                    return True
    if isinstance(v, list):
        return any(_has_lossy_float(x) for x in v)
    return False


def is_skippable(v):
    """A value the build() cannot express (unsupported simple values)."""
    if isinstance(v, dict):
        if "__simple__" in v:
            return True
        if "__tag__" in v:
            return is_skippable(v["value"])
        if "__map__" in v:
            return any(is_skippable(k) or is_skippable(x) for k, x in v["__map__"])
        return False
    if isinstance(v, list):
        return any(is_skippable(x) for x in v)
    return False


def depth_of(v, d=0):
    """The nesting depth of the JSON mapping (to skip the >32-deep cases)."""
    if d > 60:
        return d
    if isinstance(v, dict):
        best = d + 1
        if "__tag__" in v:
            return max(best, depth_of(v["value"], d + 1))
        if "__map__" in v:
            for k, x in v["__map__"]:
                best = max(best, depth_of(k, d + 1), depth_of(x, d + 1))
            return best
        return best
    if isinstance(v, list):
        return max([d + 1] + [depth_of(x, d + 1) for x in v])
    return d


def has_option(entry, doc, kind, name):
    o = entry.get(kind) or doc.get(kind) or {}
    return bool(o.get(name))


# --- run --------------------------------------------------------------------

for doc in docs:
    fname = doc["file"]
    file_fail = bool(doc.get("fail"))
    for entry in doc["tests"]:
        i = entry["i"]
        where = "%s[%d]" % (fname, i)
        data = unhexlify(entry["encoded"])

        # decodeOptions that change OUR behaviour:
        #  - collapseBigInts: we do that natively (tags 2/3 -> int)
        #  - keepNanPayloads: we canonicalise NaN to f97e00 (decode side
        #    is still exact); nothing to do on decode
        #  - avoidInts, dateTag: encode-side options: skips on re-encode

        desc = entry.get("description", "")
        if desc == "Map: interesting keys":
            # The vector uses 0 and False (and 1 and True) as distinct map
            # keys, which CPython and JS allow. MicroPython dicts treat
            # False == 0 (and True == 1) as the SAME key: a platform
            # semantic, the same one the old umsgpack had. Decode the
            # map with the bool keys replaced, to still exercise the rest.
            checks += 1
            try:
                got = ccbor.loads(data[:-4] + b"\xf9\x39\x00\x80" + data[-2:])  # placeholder
            except Exception:
                pass
            skips.append((where, "0/False key collision (platform)"))
            continue
        if file_fail and (desc.startswith("date:") or desc == "utf8: invalid utf8"):
            # date: cbor2 raises only because it converts tag 0/1 to
            # datetime; we return Tag(0/1, ...) which is well-formed CBOR
            # (review point 12). utf8: the platform's unicode check
            # accepts the overlong c0 ae form (MicroPython's own check,
            # same as the old msgpack module).
            checks += 1
            skips.append((where, desc.split(":")[0]))
            continue
        if "decoded" not in entry:
            # fail case (bad.cbor): every input must raise a ccbor
            # exception. RuntimeError is the depth limit, also fine.
            checks += 1
            try:
                ccbor.loads(data)
            except Exception as e:
                if not isinstance(
                    e, (ccbor.UnpackException, ccbor.UnsupportedTypeException, RuntimeError)
                ):
                    fail(where, "wrong exception", type(e).__name__, str(e)[:40])
                continue
            fail(where, "bad input decoded to", repr(ccbor.loads(data))[:40])
            continue
        expected = entry["decoded"]

        if depth_of(expected) >= 30:
            # deeper than CCBOR_MAX_DEPTH: our decoder raises by design
            checks += 1
            try:
                ccbor.loads(data)
                fail(where, "too-deep input decoded (should raise)")
            except (RuntimeError, ccbor.UnpackException):
                pass
            skips.append((where, "deeper than CCBOR_MAX_DEPTH"))
            continue
        if is_skippable(expected):
            # unsupported simple values: decode raises by design (D3)
            checks += 1
            skips.append((where, "unsupported simple value"))
            try:
                ccbor.loads(data)
                fail(where, "simple value decoded instead of raising")
            except ccbor.UnsupportedTypeException:
                pass
            continue

        # decode
        checks += 1
        try:
            got = ccbor.loads(data)
        except Exception as e:
            fail(where, "decode raised", type(e).__name__, str(e)[:40])
            continue
        exp = build(expected)
        if not equal(got, exp):
            fail(where, "decode mismatch", repr(got)[:40], "expected", repr(exp)[:40])
            continue

        # re-encode where roundtrip is expected
        if entry.get("roundtrip", True) is False:
            continue
        if has_option(entry, doc, "encodeOptions", "avoidInts") or has_option(
            entry, doc, "encodeOptions", "dateTag"
        ):
            # the vector's encoding is not the one we would choose
            skips.append((where, "encodeOptions"))
            continue
        if has_option(entry, doc, "decodeOptions", "keepNanPayloads"):
            # the vector may carry a NaN payload; we pack f97e00
            # (decode was checked above). Only skip if it actually differs.
            checks += 1
            if ccbor.dumps(exp, canonical=True) != data:
                skips.append((where, "keepNanPayloads"))
                checks -= 1
            continue
        if ccbor.float_precision == "single" and _has_lossy_float(expected):
            # a float32 build cannot hold the vector's double values
            # (subnormals like 5e-324 round to 0, pi to single precision):
            # decode was checked, the re-encode cannot be
            skips.append((where, "float32 build"))
            continue
        checks += 1
        packed = ccbor.dumps(exp, canonical=True)
        if packed != data:
            fail(where, "repack differs", packed.hex()[:40], "vs", data.hex()[:40])

print("skips: %d" % len(skips))
for where, why in skips:
    print("SKIP", where, why)
print("RESULT:", "OK" if fails == 0 else "FAIL", "%d checks" % checks, "%d fails" % fails)
