# refcbor: a small, pure-Python CBOR codec (RFC 8949) for the differential
# test against the C module. Correctness over speed: it is the oracle, and it
# is itself validated against cbor2 on the host before being trusted.
#
# Same API surface as ccbor: dumps/dump/loads/load, Tag, the options
# (canonical, force_float_precision, use_tuple, use_ordered_dict,
# allow_invalid_utf8), the same exception names and messages.
#
# Runs under MicroPython and CPython.

import struct

try:
    from micropython import const
except ImportError:

    def const(x):
        return x


_MT_UINT = const(0x00)
_MT_NEGINT = const(0x20)
_MT_BYTES = const(0x40)
_MT_TEXT = const(0x60)
_MT_ARRAY = const(0x80)
_MT_MAP = const(0xA0)
_MT_TAG = const(0xC0)
_MT_7 = const(0xE0)

_AI_1 = const(24)
_AI_2 = const(25)
_AI_4 = const(26)
_AI_8 = const(27)
_AI_INDEF = const(31)

_BREAK = const(0xFF)


class CCBORException(Exception):
    pass


class PackException(CCBORException):
    pass


class UnpackException(CCBORException):
    pass


class UnsupportedTypeException(PackException):
    pass


class InsufficientDataException(UnpackException):
    pass


class InvalidStringException(UnpackException):
    pass


class InvalidCodeException(UnpackException):
    pass


class UnhashableKeyException(UnpackException):
    pass


class DuplicateKeyException(UnpackException):
    pass


class Tag:
    "a CBOR tag"

    def __init__(self, tag, value=None):
        self.tag = tag
        self.value = value

    def __repr__(self):
        return "Tag(%r, %r)" % (self.tag, self.value)

    def __eq__(self, other):
        return isinstance(other, Tag) and self.tag == other.tag and self.value == other.value

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash(self.tag) ^ hash(self.value)


# ---------------------------------------------------------------------------
# Packing


def _head(out, mt, arg):
    "major type + argument, preferred (minimal) width"
    if arg < 24:
        out.append(mt | arg)
    elif arg < 0x100:
        out.append(mt | _AI_1)
        out.append(arg)
    elif arg < 0x10000:
        out.append(mt | _AI_2)
        out += struct.pack(">H", arg)
    elif arg < 0x100000000:
        out.append(mt | _AI_4)
        out += struct.pack(">I", arg)
    else:
        out.append(mt | _AI_8)
        out += struct.pack(">Q", arg)


def _f16_bits(f32):
    "the half-precision bits of a float32 value, round-to-nearest-even"
    x = struct.pack(">f", f32)
    u = struct.unpack(">I", x)[0]
    sign = (u >> 16) & 0x8000
    exp = ((u >> 23) & 0xFF) - 127 + 15
    man = u & 0x7FFFFF
    if ((u >> 23) & 0xFF) == 0xFF:
        return sign | 0x7C00 | (man >> 13)  # inf / NaN
    if exp >= 0x1F:
        return sign | 0x7C00  # overflow -> inf
    if exp <= 0:
        if exp < -10:
            return sign  # too small: rounds to zero
        m = man | 0x800000
        shift = 14 - exp
        half = 1 << (shift - 1)
        val = m >> shift
        if (m & half) and ((m & (half - 1)) or (val & 1)):
            val += 1
        return sign | val
    val = (exp << 10) | (man >> 13)
    rem = man & 0x1FFF
    if rem > 0x1000 or (rem == 0x1000 and ((man >> 13) & 1)):
        val += 1
        if val & 0x7C00:
            val = (exp + 1) << 10
    return sign | val


def _f32_from_f16(h):
    "expand half-precision bits to a float32 value"
    sign = (h & 0x8000) << 16
    exp = (h >> 10) & 0x1F
    man = h & 0x3FF
    if exp == 0:
        if man == 0:
            u = sign
        else:
            e = 127 - 15 + 1
            while not (man & 0x400):
                man <<= 1
                e -= 1
            man &= 0x3FF
            u = sign | (e << 23) | (man << 13)
    elif exp == 0x1F:
        u = sign | 0xFF << 23 | (man << 13)
    else:
        u = sign | ((exp - 15 + 127) << 23) | (man << 13)
    return struct.unpack(">f", struct.pack(">I", u))[0]


def _pack_float(out, f, fprec):
    "smallest exact width, or the forced one; NaN -> f97e00"
    d = struct.pack(">d", f)
    if fprec == "half":
        out.append(0xF9)
        out += struct.pack(">H", _f16_bits(f))
        return
    if fprec == "single":
        out.append(0xFA)
        out += struct.pack(">f", f)
        return
    if fprec == "double":
        out.append(0xFB)
        out += d
        return
    # native: smallest exact
    if f != f:  # NaN (never equal to itself): payload is not kept
        out.append(0xF9)
        out += b"\x7e\x00"
        return
    try:
        f32v = struct.pack(">f", f)
        f32 = struct.unpack(">f", f32v)[0]
    except OverflowError:
        f32 = None  # beyond single range: never exact
    if f32 is not None and f32 == f:
        h = _f16_bits(f32)
        if _f32_from_f16(h) == f32:
            out.append(0xF9)
            out += struct.pack(">H", h)
            return
        out.append(0xFA)
        out += f32v
        return
    out.append(0xFB)
    out += d


def _bignum_bytes(n):
    "the big-endian magnitude bytes of a non-negative int"
    out = []
    while n:
        out.append(n & 0xFF)
        n >>= 8
    return bytes(reversed(out)) if out else b""


def _from_bignum_bytes(b):
    "int from big-endian magnitude bytes"
    n = 0
    for c in b:
        n = (n << 8) | c
    return n


def _pack(out, o, opts, depth):
    if depth > 32:
        raise RuntimeError("maximum recursion depth exceeded")
    if o is None:
        out.append(0xF6)
        return
    if o is Ellipsis:
        out.append(0xF7)
        return
    if o is True or o is False:
        out.append(0xF5 if o else 0xF4)
        return
    t = type(o)
    if t is int:
        if o >= 0:
            if o < 0x10000000000000000:
                _head(out, _MT_UINT, o)
            else:
                _head(out, _MT_TAG, 2)
                _pack_str(out, _bignum_bytes(o), _MT_BYTES)
        else:
            n = -1 - o  # the mt1 argument
            if n < 0x10000000000000000:
                _head(out, _MT_NEGINT, n)
            else:
                _head(out, _MT_TAG, 3)
                _pack_str(out, _bignum_bytes(n), _MT_BYTES)
        return
    if t is float:
        _pack_float(out, o, opts.get("force_float_precision"))
        return
    if t is str:
        _pack_str(out, o.encode("utf-8"), _MT_TEXT)
        return
    if t is bytes:
        _pack_str(out, o, _MT_BYTES)
        return
    if t is bytearray:
        _pack_str(out, bytes(o), _MT_BYTES)
        return
    if t is Tag:
        if o.tag < 0 or o.tag >= 0x10000000000000000:
            raise ValueError("Tag number out of range")
        _head(out, _MT_TAG, o.tag)
        _pack(out, o.value, opts, depth + 1)
        return
    if t is tuple:
        o = list(o)
    if t is list:
        _head(out, _MT_ARRAY, len(o))
        for x in o:
            _pack(out, x, opts, depth + 1)
        return
    if t is dict or (hasattr(o, "keys") and hasattr(o, "items")):
        _pack_map(out, o, opts, depth)
        return
    raise UnsupportedTypeException(str(type(o)))


def _pack_str(out, data, mt):
    _head(out, mt, len(data))
    out += data if isinstance(data, bytes) else bytes(data)


def _encoded_key(k, opts):
    tmp = []
    _pack(tmp, k, opts, 0)
    return bytes(tmp)


def _pack_map(out, d, opts, depth):
    items = list(d.items())
    if opts.get("canonical") and len(items) > 1:
        # RFC 8949 4.2.3 length-first order (what cbor2 implements)
        items.sort(key=lambda kv: (_len_enc(kv[0], opts), _encoded_key(kv[0], opts)))
    _head(out, _MT_MAP, len(items))
    for k, v in items:
        _pack(out, k, opts, depth + 1)
        _pack(out, v, opts, depth + 1)


def _len_enc(k, opts):
    return len(_encoded_key(k, opts))


def dumps(obj, **opts):
    out = []
    _pack(out, obj, opts, 0)
    return bytes(out)


def dump(obj, fp, **opts):
    fp.write(dumps(obj, **opts))


# ---------------------------------------------------------------------------
# Unpacking


class _Reader:
    def __init__(self, data):
        self.d = data
        self.i = 0

    def take(self, n):
        if len(self.d) - self.i < n:
            raise InsufficientDataException()
        out = self.d[self.i : self.i + n]
        self.i += n
        return out

    def byte(self):
        return self.take(1)[0]


def _arg(r, ai):
    "the definite-length argument of a header byte"
    if ai < 24:
        return ai
    if ai == _AI_1:
        return r.byte()
    if ai == _AI_2:
        return struct.unpack(">H", r.take(2))[0]
    if ai == _AI_4:
        return struct.unpack(">I", r.take(4))[0]
    if ai == _AI_8:
        return struct.unpack(">Q", r.take(8))[0]
    raise InvalidCodeException("invalid code 0x%02x" % (0xE0 | ai))


def _list_to_tuple(o):
    "A list used as a map key becomes a tuple, recursively (as the C module)."
    if isinstance(o, list):
        return tuple(_list_to_tuple(x) for x in o)
    return o


def _unpack(r, opts, depth, key=False):
    if depth > 32:
        raise RuntimeError("maximum recursion depth exceeded")
    ic = r.byte()
    mt = ic & 0xE0
    ai = ic & 0x1F
    if ai >= 28 and ai <= 30:
        raise InvalidCodeException("invalid code 0x%02x" % ic)
    if ic == _BREAK:
        raise InvalidCodeException("invalid code 0xff")
    if ai == _AI_INDEF and mt < _MT_BYTES or (ai == _AI_INDEF and mt == _MT_TAG):
        raise InvalidCodeException("invalid code 0x%02x" % ic)
    if mt == _MT_UINT:
        return _arg(r, ai)
    if mt == _MT_NEGINT:
        n = _arg(r, ai)
        return -1 - n if n < 0x10000000000000000 else -(n + 1)
    if mt == _MT_BYTES or mt == _MT_TEXT:
        if ai == _AI_INDEF:
            chunks = []
            while True:
                cic = r.byte()
                if cic == _BREAK:
                    break
                cmt = cic & 0xE0
                cai = cic & 0x1F
                if cmt != mt or cai == _AI_INDEF or cai >= 28:
                    raise InvalidCodeException("invalid code 0x%02x" % cic)
                chunks.append(r.take(_arg(r, cai)))
            data = b"".join(chunks)
        else:
            data = r.take(_arg(r, ai))
        if mt == _MT_TEXT:
            try:
                s = data.decode("utf-8")
            except UnicodeError:  # CPython: UnicodeDecodeError
                if opts.get("allow_invalid_utf8"):
                    return data
                raise InvalidStringException("unpacked string is invalid utf-8")
            if key and opts.get("reuse_qstr"):
                return s  # (MicroPython qstr reuse is a C-module optimisation)
            return s
        return data
    if mt == _MT_ARRAY:
        if ai == _AI_INDEF:
            out = []
            while True:
                if r.d[r.i : r.i + 1] == b"\xff":
                    r.i += 1
                    break
                out.append(_unpack(r, opts, depth + 1))
            return tuple(out) if opts.get("use_tuple") else out
        n = _arg(r, ai)
        out = []
        for _ in _loop(n):  # no range(): n may exceed the machine word
            out.append(_unpack(r, opts, depth + 1))
        return tuple(out) if opts.get("use_tuple") else out
    if mt == _MT_MAP:
        if ai == _AI_INDEF:
            d = {} if not opts.get("use_ordered_dict") else _odict()
            while True:
                if r.d[r.i : r.i + 1] == b"\xff":
                    r.i += 1
                    break
                k = _list_to_tuple(_unpack(r, opts, depth + 1, True))
                try:
                    hash(k)  # explicit, like the C module
                    if k in d:
                        raise DuplicateKeyException('"%s"' % k)
                except TypeError:
                    raise UnhashableKeyException('"%s"' % (k,))
                d[k] = _unpack(r, opts, depth + 1)
            return d
        n = _arg(r, ai)
        d = {} if not opts.get("use_ordered_dict") else _odict()
        for _ in _loop(n):
            k = _list_to_tuple(_unpack(r, opts, depth + 1, True))
            try:
                hash(k)  # explicit, like the C module's hashability check
                if k in d:
                    raise DuplicateKeyException("\"%s\" (<class '%s'>)" % (k, type(k).__name__))
            except TypeError:
                raise UnhashableKeyException('"%s"' % (k,))
            d[k] = _unpack(r, opts, depth + 1)
        return d
    if mt == _MT_TAG:
        n = _arg(r, ai)
        if n == 2 or n == 3:
            # native bignum: content must be a byte string
            cic = r.byte()
            cmt = cic & 0xE0
            cai = cic & 0x1F
            if cmt != _MT_BYTES or cai == _AI_INDEF or (cai >= 28 and cai <= 30):
                raise InvalidCodeException("invalid code 0x%02x" % cic)
            mag = _from_bignum_bytes(r.take(_arg(r, cai)))
            return mag if n == 2 else -1 - mag
        content = _unpack(r, opts, depth + 1)
        if n == 55799:
            return content  # self-describe: stripped
        return Tag(n, content)
    # mt == 0xE0
    if ai < 20:
        raise UnsupportedTypeException("unsupported simple value %d" % ai)
    if ai == 20:
        return False
    if ai == 21:
        return True
    if ai == 22:
        return None
    if ai == 23:
        return Ellipsis
    if ai == 24:
        sv = r.byte()
        if sv < 32:
            raise InvalidCodeException("invalid code 0x%02x" % ic)
        raise UnsupportedTypeException("unsupported simple value %d" % sv)
    if ai == 25:
        return _f32_from_f16(struct.unpack(">H", r.take(2))[0])
    if ai == 26:
        return struct.unpack(">f", r.take(4))[0]
    if ai == 27:
        return struct.unpack(">d", r.take(8))[0]
    raise InvalidCodeException("invalid code 0x%02x" % ic)


def _loop(n):
    "Count to n (which may exceed the machine word) without range()."
    i = 0
    while i < n:
        yield i
        i += 1


def _odict():
    try:
        from collections import OrderedDict

        return OrderedDict()
    except ImportError:
        raise NotImplementedError("OrderedDict not available")


def loads(data, **opts):
    r = _Reader(data)
    return _unpack(r, opts, 0)


def load(fp, **opts):
    return loads(fp.read(), **opts)
