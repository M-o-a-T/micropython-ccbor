# register(): the direct way to tag a class - built-in or user-defined - with
# plain pack/unpack callables. Registry shapes, MRO lookup, pack=None /
# unpack=None, validation, options plumbing, depth, and Tag's immunity.
#
# The registrations are kept non-overlapping: a packer's return value is
# packed by the normal rules, so a packer that returns an int while int is
# registered would nest tags (that is its own test below).
try:
    import ccbor
except ImportError:
    print("SKIP")
    raise SystemExit

m = ccbor


def show(f, *args, **kw):
    try:
        r = f(*args, **kw)
        print("returned", r)
    except Exception as e:
        print(type(e).__name__, e)


# --- built-in types: the whole point of register() ---

m.register(
    complex, 0x50, lambda o, options: [o.real, o.imag], lambda d, options: complex(d[0], d[1])
)
print(m.dumps(1 + 2j).hex() == "d85082f93c00f94000")

m.register(set, 0x51, lambda s, options: sorted(s), lambda d, options: set(d))
p = m.dumps({3, 1, 2})
print(p.hex() == "d85183010203", m.loads(p) == {1, 2, 3})

# the native encoding is overridden: a plain tuple would be 82 01 02
m.register(tuple, 0x52, lambda t, options: list(t), lambda d, options: tuple(d))
print(m.dumps((1, 2)).hex() == "d852820102")

# pack=None keeps the native encoding (registered for decoding only)
m.register(bytes, 0x53, None, lambda d, options: b"decoded:" + d)
print(m.dumps(b"native").hex() == "466e6174697665")
print(m.loads(bytes.fromhex("d8534178")) == b"decoded:x")

# unpack=None: encoding only; decoding the tag is NotImplementedError
m.register(bytearray, 0x54, lambda b, options: b"N", None)
print(m.dumps(bytearray(b"xyz")).hex() == "d854414e")
show(m.loads, bytes.fromhex("d854f6"))


# --- user classes and the MRO ---


class Point:
    def __init__(self, x, y):
        self.x, self.y = x, y


m.register(Point, 0x55, lambda o, options: [o.x, o.y], lambda d, options: Point(*d))
pt = Point("a", "b")
print(m.dumps(pt).hex() == "d8558261616162")
q = m.loads(m.dumps(pt))
print(type(q) is Point, q.x, q.y)


class Sub(Point):
    pass


class SubSub(Sub):
    pass


# a subclass is caught through the MRO; the packer of the base runs
print(m.dumps(Sub("c", "d")).hex() == "d8558261636164")
print(m.loads(m.dumps(SubSub("e", "f"))).x)

# an instance that already existed when the class was registered
old = Point("g", "h")
show(m.dumps, old)


# multiple inheritance: the first base in the MRO order wins
class A:
    pass


class B:
    pass


class C(A, B):
    pass


m.register(A, 0x56, lambda o, options: "A", lambda d, options: ("A", d))
m.register(B, 0x57, lambda o, options: "B", lambda d, options: ("B", d))
print(m.dumps(C()).hex(), m.loads(m.dumps(C())))
print(m.dumps(B()).hex() == "d8576142")


# --- what the MRO means for the native types ---

try:
    from collections import OrderedDict, namedtuple

    # OrderedDict has parent=dict: it is caught, as isinstance always was.
    # (The packer returns the items as a list of 2-lists: tuples are also
    # registered, and the order is the dict's own.)
    m.register(
        dict, 0x59, lambda o, options: [list(kv) for kv in o.items()], lambda d, options: dict(d)
    )
    print(m.dumps(OrderedDict([("z", "p"), ("a", "q")])).hex() == "d8598282617a61708261616171")
    # a namedtuple has the tuple layout: caught through the tuple parent
    NT = namedtuple("NT", ("x", "y"))
    print(m.dumps(NT("x", "y")).hex() == "d8528261786179")
except ImportError:
    print("no collections")

# bool is not a subclass of int here (no parent slot): True packs as true
m.register(int, 0x58, lambda o, options: "I", lambda d, options: d)
print(m.dumps(3).hex() == "d8586149", m.dumps(True).hex() == "f5")


class I(int):
    pass


# a Python subclass of int IS caught (parent slot)
print(m.dumps(I(300)).hex() == "d8586149")


# a Python subclass of a type that is NOT registered packs natively
# (the pk_subclass path): list has no registration here
class L(list):
    pass


print(m.dumps(L(["x", "y"])).hex() == "8261786179")


# --- errors and validation ---


class Fresh:
    pass


show(lambda: m.register(Fresh, 0x55, None, None))  # tag taken
show(lambda: m.register(Point, 0x5A, None, None))  # class taken
show(lambda: m.register(Fresh, 2, None, None))  # native bignum tags
show(lambda: m.register(Fresh, 3, None, None))
show(lambda: m.register(Fresh, -1, None, None))
show(lambda: m.register(Fresh, 1, 1, None))  # pack not callable
show(lambda: m.register(Fresh, 1, None, 1))  # unpack not callable
show(lambda: m.register(Fresh, "1", None, None))  # not an int
m.register(Fresh, 0x5A, None, None)  # fine

# the registry shapes, as documented
print(m.custom[Point][0] == 0x55, len(m.custom[Point]) == 2)
print(m.tags[0x55] is not None, m.tags[0x54] is None)
print(m.custom[tuple][0] == 0x52)

# options plumbing: pack gets the dumps options, unpack the load options
seen = []


def opt_pack(o, options):
    seen.append(("pack", sorted(options.items())))
    return 0


def opt_unpack(d, options):
    seen.append(("unpack", sorted(options.items())))
    return d


class Opt:
    pass


m.register(Opt, 0x5B, opt_pack, opt_unpack)
m.dumps(Opt(), canonical=True, custom=7)
data = m.dumps(Opt())  # seen grows again: pack the content first
m.loads(data, use_tuple=True, custom=9)
print(seen[0])
print(seen[2][0], seen[2][1])

# a packer whose result is packed by the normal rules: int IS registered,
# so returning an int from another packer nests the tags (one level only)
m.register(memoryview, 0x5D, lambda v, options: 7, None)
print(m.dumps(memoryview(b"ab")).hex() == "d85dd8586149")


# depth: a pack that returns its own class recurses; the limit is an error
class Loop:
    pass


m.register(Loop, 0x5C, lambda o, options: Loop(), None)
show(m.dumps, Loop())

# Tag instances always pack generically, even with their number registered
# (their CONTENT packs by the normal rules)
print(m.dumps(m.Tag(0x55, None)).hex() == "d855f6")
print(m.dumps(m.Tag(0x55, ["k"])).hex() == "d85581616b")


# the registry lives in the ccbor module: hiding it removes the registry,
# restoring it brings the tags back, a damaged dict is an error
import sys

reg = sys.modules["ccbor"]
saved_tags = reg.tags
sys.modules["ccbor"] = 1
show(m.dumps, Point("i", "j"))
show(m.loads, bytes.fromhex("d855f6"))
sys.modules["ccbor"] = reg
print(m.dumps(Point("i", "j")).hex() == "d855826169616a")
reg.tags = 1
show(m.loads, bytes.fromhex("d855f6"))
reg.tags = saved_tags
print(m.loads(bytes.fromhex("d855826169616a")).x)
