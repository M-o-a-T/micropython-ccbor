# Differential test: ccbor (C) against refcbor (pure Python).
#
# Runs under MicroPython (unix port or a board) with both modules importable.
# Random objects are packed by both modules and must give the same bytes or
# the same exception (type and message). Random, valid, mutated and truncated
# byte strings are unpacked by both and must give equal objects or the same
# exception. The PRNG is seeded, so a run is reproducible.
#
#   micropython diff_ccbor.py [quick] [seed=N]
#
# Last line: "RESULT: OK <n> checks" or "RESULT: FAIL <n> mismatches".

import sys

import ccbor
import refcbor

QUICK = "quick" in sys.argv
SEED = 1
for a in sys.argv:
    if a.startswith("seed="):
        SEED = int(a[5:])

import random  # noqa: E402

random.seed(SEED)

N_PACK = 400 if QUICK else 2000
N_UNPACK = 1000 if QUICK else 4000
MAX_DEPTH = 4 if QUICK else 5
BIG = not QUICK  # strings and containers with 16/32-bit length headers

checks = 0
fails = 0
# refcbor is the oracle but is allowed to differ where the C module is
# deliberately better: none currently. Both must agree on everything.
kinds = {}  # mismatch summary: (operation, ccbor result, refcbor result) -> count


def brepr(o, n):
    "repr() truncated without building the whole string first"
    r = repr(o)
    return r[:n]


def report(kind, inp, a, b):
    global fails
    fails += 1
    if fails <= 10:
        print("MISMATCH", kind, brepr(inp, 60))
        print("  ccbor  :", brepr(a, 70))
        print("  refcbor:", brepr(b, 70))
    kinds[
        (
            kind,
            type(a).__name__ if isinstance(a, Exception) else "ok",
            type(b).__name__ if isinstance(b, Exception) else "ok",
        )
    ] = kinds.get((kind, "a", "b"), 0) + 1


def outcome(f, *args, **kw):
    try:
        return ("ok", f(*args, **kw))
    except Exception as e:
        return ("exc", e)


def same_exc(a, b):
    # the same exception NAME and message (each module has its own classes)
    return type(a).__name__ == type(b).__name__ and str(a) == str(b)


def canon(o):
    "The object as the two modules should agree on it."
    if isinstance(o, (list, tuple)):
        return [canon(x) for x in o]
    if isinstance(o, dict):
        return {canon(k): canon(v) for k, v in o.items()}
    if isinstance(o, (bytes, bytearray)):
        return bytes(o)
    if o is ccbor.Tag and False:
        pass
    if isinstance(o, ccbor.Tag):
        return ("Tag", canon(o.tag), canon(o.value))
    if isinstance(o, refcbor.Tag):
        return ("Tag", canon(o.tag), canon(o.value))
    return o


def same(a, b):
    "Two decoded objects are equal, up to Tag class, float width and NaN."
    import struct

    single = ccbor.float_precision == "single"

    def eq(x, y):
        if isinstance(x, float) or isinstance(y, float):
            if isinstance(x, float) and isinstance(y, float):
                if x != x and y != y:
                    return True  # NaN == NaN here (payloads are not kept)
                if single:
                    return struct.pack(">f", x) == struct.pack(">f", y)
                return struct.pack(">d", x) == struct.pack(">d", y)
            return False
        if isinstance(x, (list, tuple)):
            return (
                isinstance(y, (list, tuple))
                and len(x) == len(y)
                and all(eq(a1, b1) for a1, b1 in zip(x, y))
            )
        if isinstance(x, dict):
            return (
                isinstance(y, dict)
                and len(x) == len(y)
                and all(
                    any(eq(k1, k2) and eq(v1, v2) for k1, v1 in y.items()) for k2, v2 in x.items()
                )
            )
        return type(x) is type(y) and x == y

    return eq(canon(a), canon(b))


# ---------------------------------------------------------------------------
# random objects


def rint():
    r = random.getrandbits(3)
    if r == 0:
        return random.getrandbits(7)
    if r == 1:
        return random.getrandbits(16)
    if r == 2:
        return random.getrandbits(32) - (1 << 31)
    if r == 3:
        return (random.getrandbits(32) << 32) | random.getrandbits(32)
    if r == 4:
        return -1 - ((random.getrandbits(32) << 32) | random.getrandbits(32))
    if r == 5:
        return 2**64 + random.getrandbits(8)
    if r == 6:
        return -(2**64) - 1 - random.getrandbits(8)
    return (random.getrandbits(31) << 32 | random.getrandbits(32)) - (1 << 62)


def rfloat():
    import struct

    r = random.getrandbits(3)
    if r == 0:
        return struct.unpack(
            ">d", struct.pack(">Q", random.getrandbits(32) << 32 | random.getrandbits(32))
        )[0]
    if r == 1:
        return random.choice([0.0, -0.0, float("inf"), float("-inf")])
    if r == 2:
        return random.choice([0.5, 1.5, 2.5, -4.0, 65504.0, 1e10, 1.5e-8])
    if r == 3:
        return random.getrandbits(16) / 2.0
    return random.getrandbits(32) / 1e6


def rstr():
    n = random.choice([0, 1, 5, 23, 24, 255] + ([256, 1000] if BIG else []))
    return "".join(chr(random.choice((97, 233, 8364, 128512))) for _ in range(n))


def rbytes():
    n = random.choice([0, 1, 5, 23, 24, 255] + ([256, 3000] if BIG else []))
    return bytes(random.getrandbits(8) for _ in range(n))


class TagM:
    "A tag marker: converts to the Tag class of whichever module packs it."

    def __init__(self, n, value):
        self.n = n
        self.value = value


def adapt(o, mod):
    "Replace TagM markers with the module's own Tag class, recursively."
    if isinstance(o, TagM):
        return mod.Tag(o.n, adapt(o.value, mod))
    if isinstance(o, list):
        return [adapt(x, mod) for x in o]
    if isinstance(o, dict):
        return {adapt(k, mod): adapt(v, mod) for k, v in o.items()}
    if isinstance(o, (ccbor.Tag, refcbor.Tag)):
        # already a concrete Tag (from a decoded object): re-wrap
        return mod.Tag(o.tag, adapt(o.value, mod))
    return o


def rtag():
    return TagM(random.choice([0, 1, 4, 5, 30, 32, 100, 55798]), None)


def robj(depth=0):
    r = random.getrandbits(4)
    if depth >= MAX_DEPTH:
        r &= 3  # scalars only past the depth cap
    if depth >= MAX_DEPTH + 2:
        return random.choice([0, 1, "k", None])  # keys: no recursion at all
    if r == 0:
        return rint()
    if r == 1:
        return rfloat()
    if r == 2:
        return random.choice([None, True, False, ...])
    if r == 3:
        return rstr()
    if r == 4:
        return rbytes()
    if r == 5:
        t = rtag()
        t.value = robj(depth + 1)
        return t
    if r < 10:
        n = random.choice([0, 1, 2, 5, 23, 24] + ([25] if BIG else []))
        return [robj(depth + 1) for _ in range(n)]
    n = random.choice([0, 1, 2, 5] + ([12] if BIG else []))
    d = {}
    for _ in range(n):
        k = robj(6)  # a scalar-ish key
        if isinstance(k, (list, bytearray, TagM)) or isinstance(k, (ccbor.Tag, refcbor.Tag)):
            k = str(k)
        if k != k:  # NaN keys are not allowed
            k = "nan"
        d[k] = robj(depth + 1)
    return d


def check_pack(obj, opts):
    global checks
    checks += 1
    a = outcome(ccbor.dumps, adapt(obj, ccbor), **opts)
    b = outcome(refcbor.dumps, adapt(obj, refcbor), **opts)
    if (
        a[0] != b[0]
        or (a[0] == "ok" and not same(a[1], b[1]))
        or (a[0] == "exc" and not same_exc(a[1], b[1]))
    ):
        report("pack", obj, a[1] if a[0] == "exc" else a[1], b[1] if b[0] == "exc" else b[1])


def check_unpack(data, opts):
    global checks
    checks += 1
    a = outcome(ccbor.loads, data, **opts)
    b = outcome(refcbor.loads, data, **opts)
    if a[0] != b[0]:
        report("unpack", data, a[1] if a[0] == "exc" else a[1], b[1] if b[0] == "exc" else b[1])
        return
    if a[0] == "exc":
        if not same_exc(a[1], b[1]):
            report("unpack-exc", data, a[1], b[1])
        return
    if not same(a[1], b[1]):
        report("unpack-obj", data, a[1], b[1])


# ---------------------------------------------------------------------------
# phase 1: pack random objects, both plain and canonical


def run_pack_phase():
    for _ in range(N_PACK):
        obj = robj()
        check_pack(obj, {})
        if random.getrandbits(2) == 0:
            check_pack(obj, {"canonical": True})


# phase 2: unpack random, mutated and truncated bytes


def mutate(data):
    data = bytearray(data)
    for _ in range(random.getrandbits(2) + 1):
        if not data:
            data.extend(bytes(1))
            continue
        i = random.getrandbits(16) % len(data)
        r = random.getrandbits(2)
        if r == 0:
            data[i] = random.getrandbits(8)
        elif r == 1:
            data = data[:i]
        elif r == 2:
            # insert CBOR-relevant bytes
            data[i:i] = bytes(
                [
                    random.choice(
                        (
                            0x1C,
                            0x1D,
                            0x1E,
                            0x1F,
                            0x3F,
                            0x5F,
                            0x7F,
                            0x9F,
                            0xBF,
                            0xFF,
                            0xF8,
                            0xC2,
                            0xC3,
                            0xD9,
                            0xDB,
                            0x18,
                            0x19,
                            0x1A,
                            0x1B,
                        )
                    )
                ]
            )
        else:
            # flip a bit in a header-ish position
            data[i] ^= 1 << random.getrandbits(3)
    return bytes(data)


def run_unpack_phase():
    corpus = []
    for _ in range(40 if QUICK else 100):
        obj = robj()
        try:
            corpus.append(ccbor.dumps(obj))
        except Exception:
            pass
    for _ in range(N_UNPACK):
        r = random.getrandbits(2)
        if r == 0:
            n = random.getrandbits(5)
            data = bytes(random.getrandbits(8) for _ in range(n))
        elif r == 1:
            data = random.choice(corpus)
        else:
            data = mutate(random.choice(corpus)) if corpus else b""
        opts = {}
        if random.getrandbits(3) == 0:
            opts["use_tuple"] = True
        check_unpack(data, opts)
        if random.getrandbits(3) == 0:
            check_unpack(data[: len(data) // 2], opts)  # truncated


run_pack_phase()
run_unpack_phase()

print("checks:", checks)
if fails:
    print("kinds:", sorted(kinds.items()))
    print("RESULT: FAIL", fails, "mismatches in", checks, "checks")
else:
    print("RESULT: OK", checks, "checks")
