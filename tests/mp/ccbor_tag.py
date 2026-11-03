# Tag types: the tag_serializable decorator, user classes and built-in types,
# and the Tag wrapper. Registry dicts and errors.
try:
    import ccbor
except ImportError:
    print("SKIP")
    raise SystemExit

m = ccbor


@m.tag_serializable(0x10)
class Blob:
    def __init__(self, data):
        self.data = data

    def packb(self):
        return self.data

    @staticmethod
    def unpackb(data, options):
        return Blob(data)


# content is a CBOR item, not raw bytes (D1): a tag number 16 is the single
# byte 0xd0 (mt6, preferred width), then the packed content
for n, content, head in (
    (0, b"", "d040"),
    (1, b"x", "d04178"),
    (2, b"xy", "d0427879"),
    (3, [1, 2, 3], "d083010203"),
    (4, {"k": "v"}, "d0a1616b6176"),
    (5, 42, "d0182a"),
    (6, "text", "d06474657874"),
    (7, None, "d0f6"),
):
    p = m.dumps(Blob(content))
    print(n, p.hex(), p.hex() == head)


class Base:
    def __init__(self, x):
        self.x = x

    def packb(self):
        return [self.x]

    @classmethod
    def unpackb(cls, data, options):
        return cls(data[0])


m.tag_serializable(0x11)(Base)


class Derived(Base):
    pass


p = m.dumps(Derived(-3))
print(p.hex(), p.hex() == "d18122")


# a built-in type, with the dumps options passed to packb
@m.tag_serializable(0x50, complex)
class Complex:
    @staticmethod
    def packb(obj, options):
        return [obj.real, obj.imag]

    @staticmethod
    def unpackb(data, options):
        return complex(data[0], data[1])


print(m.dumps(1 + 2j).hex() == "d85082f93c00f94000")


# a built-in tag type has priority over the native encoding (the umsgpack
# mpk_tuple trick); its packb may call dumps recursively
@m.tag_serializable(0x52, tuple)
class Tuple:
    @staticmethod
    def packb(obj, options):
        return list(obj)

    @staticmethod
    def unpackb(data, options):
        return tuple(data)


print(m.dumps((1, 2)).hex() == "d852820102")

# the options given to dumps reach packb
seen = []


@m.tag_serializable(0x12, float)
class Opt:
    @staticmethod
    def packb(obj, options):
        seen.append(sorted(options.items()))
        return 0


m.dumps(1.5, canonical=True, custom_option=7)
print(seen[0])

# the registry: {class: (tag, packfn)} and {tag: unpackfn}
print(sorted(int(k) for k in m.tags))
print(m.custom[Blob][0], m.custom[complex][0])
# a class with packb but no unpackb registers as pack-only (tags value None)
print(m.tags[0x12] is None, m.tags[0x50] is not None)


# errors
def show(f, *args):
    try:
        print("returned", f(*args))
    except Exception as e:
        print(type(e).__name__, e)


show(lambda: m.tag_serializable(0x10)(type("Again", (), {})))
show(lambda: m.tag_serializable(0x13)(Blob))
show(lambda: m.tag_serializable(2)(type("BigNum", (), {})))
show(lambda: m.tag_serializable(3)(type("NegBigNum", (), {})))
show(lambda: m.tag_serializable(-1)(type("Neg", (), {})))
show(lambda: m.tag_serializable(2**64)(type("Huge", (), {})))
show(lambda: m.tag_serializable("1")(type("Str", (), {})))


@m.tag_serializable(0x14)
class NoMethods:
    pass


show(m.dumps, NoMethods())


# packb returning an unencodable object
@m.tag_serializable(0x15)
class Bad:
    def packb(self):
        return object()


show(m.dumps, Bad())

# Tag wrapper (D2)
print(repr(m.Tag(1, 2)))
t = m.Tag(5)
print(t.tag, t.value)
print(m.Tag(1, 2) == m.Tag(1, 2), m.Tag(1, 2) == m.Tag(2, 1))
print(m.Tag(1, 2) != m.Tag(1, 3))
print(m.dumps(m.Tag(1, 2)).hex())
print(hash(m.Tag(1, 2)) == hash(m.Tag(2, 1)))
print({m.Tag(1, "k"): "v"}[m.Tag(1, "k")])
show(m.Tag, "x")
show(m.Tag, -1)
show(m.Tag, 2**65)
