# Exceptions: hierarchy, types and messages.
try:
    import ccbor
except ImportError:
    print("SKIP")
    raise SystemExit

m = ccbor
print(issubclass(m.UnsupportedTypeException, m.PackException))
for e in (
    m.InsufficientDataException,
    m.InvalidStringException,
    m.InvalidCodeException,
    m.UnhashableKeyException,
    m.DuplicateKeyException,
):
    print(e.__name__, issubclass(e, m.UnpackException), issubclass(e, Exception))


def show(f, *args, **kw):
    try:
        print("returned", f(*args, **kw))
    except Exception as e:
        print(type(e).__name__, e)


# packing
show(m.dumps, {1, 2})
show(m.dumps, object())
show(m.dumps, [1, 2, {3: complex(1, 2)}])
show(m.dumps, 1.5, force_float_precision="half")
show(m.dumps, 1, force_float_precision="half")  # only an error if a float is packed
show(m.dumps, 1.5, force_float_precision="quarter")
show(m.dumps, 1, force_float_precision="quarter")  # only an error if a float is packed
show(m.dumps)
show(m.dumps, 1, 2)
show(m.dumps, 2**200)  # beyond CCBOR_MAX_BIGNUM_LIMBS

# unpacking
show(m.loads, b"")
show(m.loads, b"\x82\x01")  # truncated array
show(m.loads, b"\x65abc")  # truncated string
show(m.loads, b"\x18")  # truncated uint8
show(m.loads, b"\x1c")  # invalid additional info
show(m.loads, b"\x81\x1c")
show(m.loads, b"\xa2\x61\x61\x01\x61\x61\x02")  # duplicate keys
show(m.loads, b"\xa2\x01\x01\x01\x02")
show(m.loads, b"\xa1\x80\x01")  # dict as a key: unhashable
show(m.loads, b"\xa1\x81\x80\x01")
show(m.loads, b"\xf0")  # unassigned simple value 16
show(m.loads, b"\xf8\x20")  # unassigned simple value 32
show(m.loads, b"\xf8\x18")  # reserved: 1-byte simple below 32
show(m.loads, b"\xff")  # stray BREAK
show(m.loads, b"\x1f")  # indefinite int
show(m.loads, b"\x3f")  # indefinite negative int
show(m.loads, b"\xc1\x1f")  # indefinite tag number
show(m.loads, b"\x5f\x01\xff")  # indefinite bytes with a wrong chunk
show(m.loads, b"\x7f\x7f\xff")  # indefinite text with an indefinite chunk
show(m.loads, b"\x9f")  # indefinite without BREAK
show(m.loads, b"\xbf\x61\x61\xff")  # indefinite map, missing value
show(m.loads, b"\xa1\xff\x01")  # BREAK as a key
show(m.loads, b"\xa1\x01\xff")  # BREAK as a value
show(m.loads, "abc")
show(m.loads, 123)
show(m.loads, None)

# a huge declared length is detected without allocating it
show(m.loads, b"\x7b" + b"\xff" * 8)  # text(2**64-1)
show(m.loads, b"\x5b" + b"\xff" * 8)  # bytes(2**64-1)
show(m.loads, b"\x9b" + b"\xff" * 7)  # array(2**63-1)
show(m.loads, b"\xbb" + b"\xff" * 7 + b"\x01\x01")  # map(2**63-1)

# a tag number far out of range (checked when the decorator is applied)
try:
    m.tag_serializable(2**70)(type("Huge", (), {}))
except ValueError as e:
    print("ValueError", e)

# tags 2 and 3 are the native bignum tags
try:
    m.tag_serializable(2)(type("BigNum", (), {}))
except ValueError as e:
    print("ValueError", e)

# catching the base classes
try:
    m.loads(b"\x1c")
except m.UnpackException:
    print("caught as UnpackException")
try:
    m.dumps(set())
except m.PackException:
    print("caught as PackException")
