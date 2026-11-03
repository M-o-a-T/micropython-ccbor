# Using ccbor

`ccbor` is a [CBOR (RFC 8949)](https://www.rfc-editor.org/rfc/rfc8949)
serializer for [MicroPython](https://micropython.org): a C core (the `_ccbor`
module — `dumps`, `dump`, `loads`, `load`, `Unpacker`, `Tag`) with a thin
Python package (`ccbor`) around it that holds the tag registry,
`register()`, `tag_serializable()` and `ALoader`. Its API follows the shape
of the `json` module (and of Peter Hinch's umsgpack, of which the msgpackc
1.0 module was a drop-in replacement): `dumps`, `dump`, `loads`, `load`,
streams, incremental decoding and a decorator for your own types.

- [1. Quick tour](#1-quick-tour)
- [2. Supported types](#2-supported-types)
- [3. Functions](#3-functions)
- [4. Options](#4-options)
- [5. Exceptions](#5-exceptions)
- [6. Tags: your own and built-in types](#6-tags-your-own-and-built-in-types)
- [7. Streams and incremental decoding](#7-streams-and-incremental-decoding)
- [8. Asynchronous decoding (ALoader)](#8-asynchronous-decoding-aloader)
- [9. Deterministic encoding and cbor2](#9-deterministic-encoding-and-cbor2)
- [10. Differences from cbor2](#10-differences-from-cbor2)
- [11. Limits and safety](#11-limits-and-safety)
- [12. Build-time configuration](#12-build-time-configuration)

## 1. Quick tour

```python
import ccbor

data = ccbor.dumps({"id": "sensor-01", "temp": 21.5, "ok": True, "raw": b"\x01\x02"})
print(len(data))             # 33 bytes; the same record in JSON takes 60
obj = ccbor.loads(data)      # {'id': 'sensor-01', 'temp': 21.5, ...}
```

Send `data` over MQTT, UART, a socket or store it in a file: it is plain
`bytes`, readable by any CBOR library in any language (Python's `cbor2`,
JavaScript, Go, Rust, C...). CBOR is what JSON would be if it were designed
for machines: binary, typed, with `bytes`, 64-bit integers and a standard
extension mechanism (tags).

## 2. Supported types

| Python | CBOR | Notes |
|---|---|---|
| `None` | null (22) | |
| `...` (Ellipsis) | undefined (23) | both directions |
| `bool` | true / false | |
| `int` | major types 0/1 | from `-2**64` to `2**64 - 1` in the shortest form; beyond that, bignum tags 2/3 (arbitrary precision, up to 128 bits) |
| `float` | half/single/double | the **smallest exact** width, see [options](#4-options) |
| `str` | text string (3) | UTF-8, validated on unpack |
| `bytes`, `bytearray`, `memoryview` | byte string (2) | all unpack as `bytes` (or as views, see `memory_min`) |
| `list`, `tuple` | array (4) | both unpack as `list` (or `tuple`, see `use_tuple`) |
| `dict`, `OrderedDict` | map (5) | unpack as `dict` (or `OrderedDict`, see `use_ordered_dict`) |
| `ccbor.Tag(n, v)` | tag (6) | the [tag wrapper](#6-tags-your-own-and-built-in-types) |
| anything else | tag | with a [registered tag](#6-tags-your-own-and-built-in-types) |

Subclasses of these types (and `namedtuple`) are packed like their base type.

Any other object (a `set`, a `complex`, your own class...) raises
`UnsupportedTypeException`, unless you register a tag for it.

Decoding accepts everything the RFC allows, not only what this module
produces: non-minimal integer and length encodings, indefinite-length
strings, arrays and maps (`5f`/`7f`/`9f`/`bf` ... `ff`, chunked), and
bignum tags 2/3 decode to plain `int` (on builds whose integers stop at
64 bits they come back wrapped in `Tag(2, bytes)` / `Tag(3, bytes)`).

## 3. Functions

### `ccbor.dumps(obj, **options) -> bytes`

Pack `obj` and return the bytes.

### `ccbor.dump(obj, fp, **options)`

Pack `obj` and write the bytes to `fp` in one `write()` call. `fp` is a
stream with `write()` (any file, socket, UART...).

### `ccbor.loads(data, **options) -> object`

Unpack one object from `data` (`bytes`, `bytearray`, `memoryview`...).
Trailing bytes are ignored. Raises on corrupt data; never crashes or
allocates what the data does not really contain.

### `ccbor.load(fp, **options) -> object`

Read exactly the bytes of one object from `fp`, a stream with `read(n)`
(a file, socket, UART; short reads are fine).

### `ccbor.Unpacker(**options)`

Incremental decoding: `feed(data)` appends bytes, iterating the unpacker
yields every complete object. See
[7. Streams and incremental decoding](#7-streams-and-incremental-decoding).

### `ccbor.register(cls, tag, pack=None, unpack=None)`

Registers `cls` — **any** class: your own or a built-in one (`complex`,
`set`, `tuple`...) — for a CBOR tag number, with plain callables:
`pack(obj, options)` is called when an instance of the class (or of a
subclass) is packed, and `unpack(content, options)` when the tag number
is decoded. `pack=None` (the default) keeps the native encoding if the
class has one; `unpack=None` means the tag is not decodable. See
[6. Tags: your own and built-in types](#6-tags-your-own-and-built-in-types).

### `ccbor.tag_serializable(tag_number, example=None)`

Class decorator, the umsgpack-style sugar over
[`register()`](#ccborregistercls-tag-packnone-unpacknone): a class with
`packb()`/`unpackb()` methods registers for a tag, or (with
`example=atype`) a built-in type does.

### `ccbor.ALoader(stream, **options)`

Asynchronous decoding of a stream, on top of `Unpacker`. See
[8. Asynchronous decoding (ALoader)](#8-asynchronous-decoding-aloader).

### Constants

```python
ccbor.Tag                    # the tag wrapper type
ccbor.float_precision        # "double" or "single": this build's float width
ccbor.custom                 # {class: (tag number, pack callable)}
ccbor.tags                   # {tag number: unpack callable or None}
ccbor.PackException          # base of the packing exceptions
ccbor.UnpackException        # base of the unpacking exceptions
```

## 4. Options

All options are keyword arguments; `Unpacker` takes the unpack options at
construction, `ALoader` passes them through to the unpacker.

### Pack options (`dumps`, `dump`)

| option | values | effect |
|---|---|---|
| `canonical` | `True` | sort map keys like [cbor2](#9-deterministic-encoding-and-cbor2) |
| `force_float_precision` | `"single"`, `"double"`, `"half"` | force the float width (with loss); the default uses the smallest exact width |

### Unpack options (`loads`, `load`, `Unpacker`, `ALoader`)

| option | values | effect |
|---|---|---|
| `use_tuple` | `True` | arrays unpack as `tuple` instead of `list` |
| `use_ordered_dict` | `True` | maps unpack as `OrderedDict` |
| `allow_invalid_utf8` | `True` | invalid UTF-8 in a text string unpacks as `bytes` instead of raising `InvalidStringException` |
| `memory_min` | bytes | `loads` only: byte strings at least this long unpack as a `memoryview` into the input instead of a copy |

`memory_min` is `loads`'s zero-copy path: `load` (a stream has no buffer
that outlives the call) and `Unpacker` (its `feed()` compacts and grows the
buffer, which would silently invalidate every view) reject it with a
`ValueError` — decode `next_raw()`'s bytes with `loads(..., memory_min=)`
instead. The views are read-only over a `bytes` input and writable over a
`bytearray` one, exactly like `memoryview(input)`; they keep the input
alive (their GC anchor is the input's buffer), and the usual `memoryview`
rule applies: resizing the input under them is not supported. Text strings
never unpack as views (they are `str`), and neither do map keys or the
chunks of an indefinite-length byte string (both must stay hashable or
assembled). On a build without `memoryview` the option is silently
ignored (everything copies); code that passes it runs on both kinds of
build unchanged.

`Unpacker` also takes:

| option | values | effect |
|---|---|---|
| `max_buffer_size` | bytes | bound on the buffered data of incomplete objects; `feed()` raises `ValueError` beyond it |

The options given to `dumps`/`loads` reach the registered packer and
unpacker methods as the `options` dict.

## 5. Exceptions

```
Exception
├── PackException                     packing (dumps, dump)
│   └── UnsupportedTypeException      no encoder for this object
└── UnpackException                   unpacking (loads, load)
    ├── InsufficientDataException     the data ends in the middle of an object
    ├── InvalidStringException        text string is not valid UTF-8
    ├── InvalidCodeException         not-well-formed CBOR (bad code, stray BREAK)
    ├── UnhashableKeyException        a map key cannot be hashed
    └── DuplicateKeyException         a map repeats a key
```

`UnsupportedTypeException` serves both directions: on pack for an object
without an encoder, on unpack for an unassigned simple value (RFC 8949
simple values beyond false/true/null/undefined are not supported).

Two `RuntimeError`s are part of the contract, not bugs: `maximum recursion
depth exceeded` (nesting or tag chains deeper than 32, see
[11. Limits and safety](#11-limits-and-safety)) and
`container changed size during packing` (a tag's `packb()` modified the
object being packed).

## 6. Tags: your own and built-in types

CBOR's extension mechanism: a small integer (0 to `2**64-1`) that says what
the item that follows is. Tags replace the extension types of the msgpack
edition, but with a real difference: **the content is a CBOR item**, not raw
bytes. Your packer returns any encodable object (a list, a map, bytes, an
int...), and your unpacker receives that object back.

The registry is `register(cls, tag, pack=None, unpack=None)`: it works for
**any** class — your own or a built-in one — with plain callables:

```python
import ccbor

def pack_point(o, options):
    return [o.x, o.y]                # packed as the tag's content

def unpack_point(data, options):
    return Point(*data)              # data is the decoded content: a list


class Point:
    def __init__(self, x, y):
        self.x, self.y = x, y


ccbor.register(Point, 100, pack_point, unpack_point)

p = Point(3, 4)
data = ccbor.dumps(p)                # d8 64 82 03 04: tag(100), [3, 4]
q = ccbor.loads(data)                # Point(3, 4)
```

- `pack(obj, options)` is called when an instance of the class — or of any
  subclass, found by following the class hierarchy — is packed. The
  `options` dict is what was passed to `dumps`. `pack=None` (the default)
  means the class is not encodable: packing raises
  `UnsupportedTypeException` (and a built-in class keeps its native
  encoding).
- `unpack(content, options)` is called when the tag number is decoded,
  with the load `options`. `unpack=None` (the default) means the tag is not
  decodable: decoding it raises `NotImplementedError` — the shape to
  register a tag you can only decode (a moat-style decoder).
- The tag number must be an integer 0 to `2**64-1`, and not 2 or 3 (the
  native bignum tags). Duplicate tag numbers and duplicate classes raise
  `ValueError` at registration.

### Built-in types

A registered built-in type is packed through the tag whenever it appears,
in priority over its native encoding (the umsgpack `mpk_*` trick):

```python
ccbor.register(complex, 80,
               lambda o, options: [o.real, o.imag],
               lambda data, options: complex(*data))

ccbor.dumps(1 + 2j)                  # d850 82 f93c00 f94000:
                                     # tag(80), [1.0, 2.0] (exact as halves)
```

The class hierarchy decides what a registration catches, exactly as
`isinstance` does: a subclass of a registered class, a Python subclass of
a registered built-in type, `OrderedDict` for `dict` and `namedtuple` for
`tuple` are all caught; `bool` is not `int` here, so a registered `int`
does not swallow `True`.

### tag_serializable, the sugar

`@ccbor.tag_serializable(tag_number)` is the umsgpack-style decorator over
`register()`: a class with `packb()` and `unpackb()` methods registers for
a tag. With `example=atype` the tag is registered for the built-in type
instead, and the class's (static) `packb(obj, options)` / `unpackb(data,
options)` become its codecs.

```python
@ccbor.tag_serializable(100)          # any number you like (0..2**64-1)
class Point:
    def __init__(self, x, y):
        self.x, self.y = x, y

    def packb(self):
        return [self.x, self.y]       # packed as the tag's content

    @staticmethod
    def unpackb(data, options):
        return Point(*data)           # data is the decoded content: a list
```

### Timestamps, the recipe

The RFC's timestamp tags are not built in (as in umsgpack's ext types):
register them yourself, exactly like any other tag:

```python
import time
import ccbor

@ccbor.tag_serializable(1)            # tag 1: seconds since the epoch
class Timestamp:
    def __init__(self, s):
        self.s = s

    def packb(self):
        return int(self.s)            # an int: the tag's content

    @staticmethod
    def unpackb(data, options):
        return Timestamp(data)         # data is the decoded int
```

### The registry

The two dicts are the registry itself, plain module globals of the `ccbor`
package (the C core reads them on every call, so registering takes effect
immediately; a soft reset discards them together with the module):

```python
ccbor.custom                 # {class: (tag number, pack callable or None)}
ccbor.tags                   # {tag number: unpack callable or None}
```

### The Tag wrapper

Tags that nobody registered decode to `Tag`, a small wrapper (compatible
with moat's `Tag` and cbor2's `CBORTag`):

```python
t = ccbor.loads(bytes.fromhex("c11a514b67b0"))
t.tag, t.value                        # 1, 1363896240
repr(t)                               # Tag(1, 1363896240)
```

A `Tag` instance also packs: `ccbor.dumps(ccbor.Tag(32, "a"))` writes
`d8 20 6161`. Registering a number for a class does not change how `Tag`
instances pack: they always encode their own number and content. Two tag
numbers are special: **2 and 3** are the native bignum tags (registering
them raises `ValueError`; their content must be a byte string), and
**55799** is the self-describe tag, stripped on decode like cbor2 does.

## 7. Streams and incremental decoding

`dump` and `load` are for whole objects on a stream. `Unpacker` is for data
that arrives in pieces:

```python
u = ccbor.Unpacker()

def on_uart_data(data):
    u.feed(data)

for obj in u:                # every complete object, as it arrives
    handle(obj)
```

- `feed(data)` appends bytes; `len(u)` is the bytes buffered but not
  consumed; `bool(u)` tells whether any are left. `data` may be any read
  buffer — `bytes`, `bytearray`, `memoryview`, sliced or not — and it is
  copied, so the source may change or go away afterwards.
- Iteration raises the same exceptions as `loads` on corrupt data; the
  offending bytes are consumed, so decoding continues with the next object.
  A not-well-formed byte is skipped: the stream can resynchronise.
- `next_raw()` returns the bytes of the next complete object without
  decoding it (`None` if there is none yet).
- `reset()` discards everything buffered.
- `max_buffer_size=` bounds the memory used by incomplete data.

## 8. Asynchronous decoding (ALoader)

`ccbor.ALoader` (in the `ccbor` package, `ccbor/aio.py`, imported on first
use) decodes the objects arriving on an asyncio `StreamReader`-like object:

```python
import asyncio
import ccbor

async def main():
    reader = asyncio.StreamReader(uart)
    async for obj in ccbor.ALoader(reader):
        handle(obj)

asyncio.run(main())
```

It takes the unpack options, and an `observer=` callable that receives the
raw bytes of every object (then `b""`, as umsgpack's ALoader did).

## 9. Deterministic encoding and cbor2

`ccbor.dumps` output is deterministic: the shortest possible integer,
length and float encodings (RFC 8949's "preferred serialization"), no
indefinite lengths, and NaN always as `f97e00`. Map keys keep the dict's
order, like CPython's `json`.

`canonical=True` additionally sorts map keys the way [cbor2](https://github.com/agronholm/cbor2)
does, which makes the bytes reproducible across runs and languages:

```python
ccbor.dumps({"b": 1, "a": 2})                    # insertion order
ccbor.dumps({"b": 1, "a": 2}, canonical=True)    # "a" first
```

With `canonical=True` the output is byte-identical to cbor2's
`dumps(obj, canonical=True)` for every value both can encode — verified
over thousands of random objects by the interoperability test.

## 10. Differences from cbor2

- Duplicate map keys raise `DuplicateKeyException` instead of keeping the
  last one. (Security: a hostile document cannot silently overwrite keys.)
- Tags 0/1 (dates) and other semantic tags are **not** converted: without a
  registration they decode to `Tag(n, content)`. `Tag` is cbor2's `CBORTag`
  in spirit: `Tag(1, 1363896240)` where cbor2 would give a `datetime`.
- Bignum tags 2/3 decode to `int` (cbor2 does the same); on builds whose
  integers stop at 64 bits they come back as `Tag(2, bytes)`.
- Unassigned simple values (0-19, 32-255) raise `UnsupportedTypeException`
  instead of returning a `CBORSimpleValue`.
- The payload is not kept: NaN always decodes to the canonical `f97e00`
  form's value, like cbor2's canonical encoding.
- 55799 (self-describe) is stripped on decode, like cbor2.

## 11. Limits and safety

- Nesting (arrays, maps and tags) is limited to **32 levels** on the way in
  and out: deeper structures raise `RuntimeError`, never a stack overflow.
- Every length in the input is checked against the data really available
  before it is used; a hostile document raises an exception instead of
  allocating memory or reading out of bounds. Fuzzed under
  AddressSanitizer and UBSan.
- Map keys are hashable or rejected (`UnhashableKeyException`); a list key
  becomes a tuple, recursively, as in the msgpack edition.
- One platform note: MicroPython dictionaries treat `False` and `0` (and
  `True` and `1`) as the **same key**, so a CBOR map using both cannot be
  represented (the conformance suite counts this one case as a skip).
- `dumps` allocates about the size of the output; `loads` allocates only
  the decoded object (see [BENCHMARKS.md](BENCHMARKS.md)).

## 12. Build-time configuration

| option | default | meaning |
|---|---|---|
| `CCBOR_MAX_DEPTH` | 32 | maximum nesting of arrays, maps and tags |
| `CCBOR_REUSE_QSTR` | 1 | 1: a map key that already exists as a qstr is reused instead of copied; 0: always copy |

Both are compile-time defines (`CFLAGS_EXTRA=-DCCBOR_MAX_DEPTH=64` for the
unix port). Each nesting level takes 80 to 128 bytes of C stack on an
ESP32: 32 levels leave room for the VM, the GC and raising an exception at
the deepest level.
