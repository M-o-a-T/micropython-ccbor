# memoryview: packing it, and the memory_min option that decodes large byte
# strings as views into the caller's buffer instead of copies.
try:
    import ccbor

    memoryview
except (ImportError, NameError):  # no memoryview on this build
    print("SKIP")
    raise SystemExit

m = ccbor

# packing a memoryview gives the same bytes as packing its content
print(m.dumps(memoryview(b"abc")) == m.dumps(b"abc"))
print(m.dumps(memoryview(b"abcdef")[2:5]) == m.dumps(b"cde"))
print(m.loads(m.dumps({"d": memoryview(b"xyz")}))["d"] == b"xyz")

# memory_min: byte strings at least that long decode as memoryviews
data = m.dumps([b"ab", b"abcdefghij"])
r = m.loads(data, memory_min=5)
print(type(r[0]).__name__, type(r[1]).__name__)
print(bytes(r[1]))

# without the option everything copies; with 1 everything views
print(type(m.loads(data)[1]).__name__)
r = m.loads(data, memory_min=1)
print(type(r[0]).__name__, type(r[1]).__name__, bytes(r[0]))

# text strings are never views (they decode to str), keys never either
r = m.loads(m.dumps(["abcdefghij"]), memory_min=1)
print(type(r[0]).__name__, r[0])
r = m.loads(m.dumps({b"0123456789": 1}), memory_min=1)
print(type(list(r.keys())[0]).__name__)

# indefinite byte strings are assembled from chunks: always copies
indef = bytes([0x5F, 0x42, 0x01, 0x02, 0x41, 0x03, 0xFF])
print(type(m.loads(indef, memory_min=1)).__name__)

# the view refers to the input: writing through it writes the input
# (bytearray input), and a view of a memoryview input keeps the offsets
ba = bytearray(m.dumps(b"0123456789"))
v = m.loads(ba, memory_min=5)
v[0] = 65
print(ba[1] == 65)
d = b"\x00" + m.dumps([b"ab", b"0123456789"])
r = m.loads(memoryview(d)[1:], memory_min=5)
print(type(r[1]).__name__, bytes(r[1]))

# a view packs back to the same bytes
d = m.dumps(b"0123456789")
print(m.dumps(m.loads(d, memory_min=5)) == d)

# the option is rejected where it cannot work: a stream has no buffer to
# refer to, and feed() may move the Unpacker's buffer under the views
import io

for bad in (
    lambda: m.load(io.BytesIO(d), memory_min=5),
    lambda: m.Unpacker(memory_min=5),
    lambda: m.loads(d, memory_min=-1),
):
    try:
        bad()
        print("accepted")
    except ValueError as e:
        print("ValueError:", e)

# feed() takes any read buffer: memoryviews (also sliced, also over a
# bytearray) work, and the bytes are copied (the source may change or go
# away afterwards)
stream = m.dumps([b"ab", b"abcdefghij", None]) + m.dumps({"k": 1})
u = m.Unpacker()
u.feed(memoryview(stream))
print("feed mv:", list(u) == [[b"ab", b"abcdefghij", None], {"k": 1}])

u = m.Unpacker()
u.feed(memoryview(stream)[len(m.dumps([b"ab", b"abcdefghij", None])) :])
print("feed mv sliced:", list(u) == [{"k": 1}])

u = m.Unpacker()
u.feed(memoryview(bytearray(stream)))
print("feed mv bytearray:", list(u) == [[b"ab", b"abcdefghij", None], {"k": 1}])

u = m.Unpacker()
out = []
i = 0
n = 0
while i < len(stream):
    n += 1
    if n % 2:
        u.feed(memoryview(stream[i : i + 5]))
    else:
        u.feed(stream[i : i + 5])
    i += 5
    out.extend(u)
print("feed mv alternating:", out == [[b"ab", b"abcdefghij", None], {"k": 1}])

ba = bytearray(stream)
u = m.Unpacker()
u.feed(memoryview(ba)[:20])
for i in range(len(ba)):
    ba[i] = 0xFF
u.feed(stream[20:])
print("feed mv copied:", list(u) == [[b"ab", b"abcdefghij", None], {"k": 1}])
