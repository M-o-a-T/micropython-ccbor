# Integers at the 64-bit limits, on builds with arbitrary precision ints and
# on builds limited to 64 bits (MICROPY_LONGINT_IMPL_LONGLONG), where values
# outside the int64 range are wrapped in a Tag (D4: no exception for
# well-formed CBOR). No literal here exceeds 63 bits, so the file compiles
# on both.
try:
    import ccbor
except ImportError:
    print("SKIP")
    raise SystemExit

# Does this build have ints wider than 64 bits?
try:
    BIG = str(int("18446744073709551616")) == "18446744073709551616"
except (OverflowError, ValueError):
    BIG = False

m = ccbor
INT64_MAX = 0x7FFFFFFFFFFFFFFF
INT64_MIN = -INT64_MAX - 1

# preferred widths: everything at the 64-bit boundary uses the 8-byte form
for v, head in (
    (INT64_MAX, b"\x1b"),
    (INT64_MIN, b"\x3b"),
    (2**62, b"\x1b"),
    (-(2**62), b"\x3b"),
    (2**32, b"\x1b"),
    (-(2**32) - 1, b"\x3b"),
):
    p = m.dumps(v)
    print(p[:1] == head, m.loads(p) == v)

print(m.dumps(INT64_MAX) == b"\x1b\x7f\xff\xff\xff\xff\xff\xff\xff")
print(m.dumps(INT64_MIN) == b"\x3b\x7f\xff\xff\xff\xff\xff\xff\xff")

# a uint64 above INT64_MAX: mt0 with the full 64-bit argument
try:
    v = m.loads(b"\x1b" + b"\xff" * 8)
    print("uint64 max:", BIG and str(v) == "18446744073709551615")
except OverflowError:
    print("uint64 max:", not BIG)

# -2**64 exactly: mt1 with argument 2**64-1
p = m.dumps(-(2**63) * 2)
print(p == b"\x3b" + b"\xff" * 8, m.loads(p) == -(2**63) * 2)

# bignums beyond 64 bits: tag 2/3, round trip on builds with big ints
if BIG:
    for v in (2**64, -(2**64) - 1, 2**100, -(2**100)):
        p = m.dumps(v)
        print(p[0] in (0xC2, 0xC3), m.loads(p) == v)
else:
    # longlong build: packs raise, decodes wrap in Tag
    try:
        m.dumps(2**64)
        print("no error")
    except m.UnsupportedTypeException:
        print("UnsupportedTypeException")
    t = m.loads(b"\xc2\x49\x01" + b"\x00" * 9)
    print(type(t).__name__, t.tag, t.value)
