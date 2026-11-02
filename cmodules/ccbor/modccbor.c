/*
 * This file is part of ccbor, a MessagePack module for MicroPython.
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Carlos Tangerino
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// ccbor: CBOR (RFC 8949) for MicroPython, written in C.
//
// The Python API follows the shape of Peter Hinch's pure-Python umsgpack
// (https://github.com/peterhinch/micropython-msgpack) - dumps/dump/loads/
// load, Unpacker, ALoader, the options - because the module began as
// msgpackc, a MessagePack accelerator with umsgpack's exact behaviour, and
// was converted to CBOR. What is CBOR's own: the wire format (major types
// 0-7, preferred encodings), tags instead of extension types (the content
// is a CBOR item, not raw bytes), bignums, half floats, indefinite lengths
// on the way in, undefined <-> Ellipsis, and a canonical= option whose
// output is byte-identical with cbor2's canonical form.
//
// Design notes:
// - The encoder writes into a single growable buffer which becomes the
//   returned bytes object without a copy.
// - The decoder reads either straight from a buffer (loads, Unpacker) without
//   copying, or from a stream (load) reading exactly the bytes it needs.
// - Nothing is static state: the tag registry lives in a heap module
//   placed in sys.modules, so a soft reset discards it with everything else.
// - Every length taken from the input is checked against the data really
//   available before it is used, so corrupt or hostile input raises an
//   exception instead of reading out of bounds or allocating huge buffers.

#include <stdint.h>
#include <string.h>

#include "py/runtime.h"
#include "py/binary.h"
#include "py/builtin.h"
#include "py/mpz.h"
#include "py/objarray.h"
#include "py/objexcept.h"
#include "py/objint.h"
#include "py/objlist.h"
#include "py/objmodule.h"
#include "py/objstr.h"
#include "py/smallint.h"
#include "py/objtuple.h"
#include "py/objtype.h"
#include "py/stackctrl.h"
#include "py/stream.h"
#include "py/unicode.h"

// Maximum nesting of arrays and maps, for both packing and unpacking. Each
// level takes 80 to 128 bytes of C stack on an ESP32, whose MicroPython task
// has 16 KiB: 32 levels leave room for the VM, the GC and raising an exception
// at the deepest level. The same limit everywhere means that data packed on
// one port can be unpacked on any other. On ports with MICROPY_STACK_CHECK the
// stack is also checked at every level. Override with -DCCBOR_MAX_DEPTH=n.
#ifndef CCBOR_MAX_DEPTH
#define CCBOR_MAX_DEPTH (32)
#endif

// Limbs of the biggest integer that packs natively as a bignum (tag 2/3).
// The buffer must cover 2**64 (5 limbs of 16 bits on builds that force
// MPZ_DIG_SIZE=16, like the unix port) and a useful margin beyond; 8 limbs
// cover 128 bits either way. Bigger values raise UnsupportedTypeException,
// like the other huge-object errors: encoding them would need a heap buffer
// for the payload, and the byte-string header (whose length depends on it)
// must be written first.
#define CCBOR_MAX_BIGNUM_LIMBS (8)

// Map keys that already exist as qstrs reuse them instead of allocating a
// copy. Keys usually do: the application writes them as literals, which are
// qstrs (msg["temp"]). Other strings are not looked up: a lookup that fails
// walks the qstr pools, which costs more than the copy (measured on ESP32-P4).
#ifndef CCBOR_REUSE_QSTR
#define CCBOR_REUSE_QSTR (1)
#endif

/******************************************************************************/
// Exceptions: the same hierarchy as umsgpack.

#define CCBOR_EXCEPTION(name, base) \
    static MP_DEFINE_CONST_OBJ_TYPE(mp_type_ccbor_##name, MP_QSTR_##name, MP_TYPE_FLAG_NONE, \
    make_new, mp_obj_exception_make_new, \
    print, mp_obj_exception_print, \
    attr, mp_obj_exception_attr, \
    parent, &base \
    )

CCBOR_EXCEPTION(PackException, mp_type_Exception);
CCBOR_EXCEPTION(UnpackException, mp_type_Exception);
CCBOR_EXCEPTION(UnsupportedTypeException, mp_type_ccbor_PackException);
CCBOR_EXCEPTION(InsufficientDataException, mp_type_ccbor_UnpackException);
CCBOR_EXCEPTION(InvalidStringException, mp_type_ccbor_UnpackException);
CCBOR_EXCEPTION(InvalidCodeException, mp_type_ccbor_UnpackException);
CCBOR_EXCEPTION(UnhashableKeyException, mp_type_ccbor_UnpackException);
CCBOR_EXCEPTION(DuplicateKeyException, mp_type_ccbor_UnpackException);

static MP_NORETURN void raise_insufficient(void) {
    mp_raise_type(&mp_type_ccbor_InsufficientDataException);
}

// A not-well-formed code: additional info 28-30 anywhere, indefinite length
// where it is invalid (mt0/mt1/mt6), a stray BREAK, a bad indefinite-string
// chunk, or a 1-byte simple value below 20 (D7).
static MP_NORETURN void raise_invalid_code(byte ic) {
    mp_raise_msg_varg(&mp_type_ccbor_InvalidCodeException,
        MP_ERROR_TEXT("invalid code 0x%02x"), ic);
}

// An unassigned simple value: 0..19 (0xe0-0xf3) or 1-byte 32..255
// (f8 20..f8 ff): well-formed CBOR, but we do not support them (D3).
static MP_NORETURN void raise_unsupported_simple(unsigned sv) {
    mp_raise_msg_varg(&mp_type_ccbor_UnsupportedTypeException,
        MP_ERROR_TEXT("unsupported simple value %u"), sv);
}

static MP_NORETURN void raise_depth(void) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("maximum recursion depth exceeded"));
}

static MP_NORETURN void raise_changed(void) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("container changed size during packing"));
}

// Raise exc_type with the message: prefix + str(obj) + suffix, the
// equivalent of an f-string in umsgpack.
static MP_NORETURN void raise_with_obj(const mp_obj_type_t *exc_type, const char *prefix,
    mp_obj_t obj, mp_print_kind_t kind, const char *suffix) {
    vstr_t vstr;
    mp_print_t print;
    vstr_init_print(&vstr, 32, &print);
    mp_print_str(&print, prefix);
    mp_obj_print_helper(&print, obj, kind);
    mp_print_str(&print, suffix);
    nlr_raise(mp_obj_new_exception_arg1(exc_type, mp_obj_new_str_from_vstr(&vstr)));
}

static MP_NORETURN void raise_unsupported_type(mp_obj_t obj) {
    raise_with_obj(&mp_type_ccbor_UnsupportedTypeException, "",
        MP_OBJ_FROM_PTR(mp_obj_get_type(obj)), PRINT_STR, "");
}

static MP_NORETURN void raise_unsupported_msg(mp_rom_error_text_t msg) {
    mp_raise_msg(&mp_type_ccbor_UnsupportedTypeException, msg);
}

// Lengths above 32 bits cannot be encoded. Only possible where size_t is
// wider than 32 bits; elsewhere the test would be "always false" (a warning).
static inline bool len_exceeds_u32(size_t n) {
    #if SIZE_MAX > 0xffffffff
    return n > 0xffffffff;
    #else
    (void)n;
    return false;
    #endif
}

/******************************************************************************/
// Tag registry (the tag_serializable decorator).
//
// Three dicts: custom {user class: tag number},
// builtins {type: (packer class, tag number)} and tags {tag number: class}
// (the shape is umsgpack's, the mechanism is CBOR's). They are kept here as
// attributes of a heap module "_ccbor_ext" stored in sys.modules: no root
// pointers are needed, and a soft reset (which empties sys.modules) drops
// them together with the classes they refer to.

static mp_obj_t ext_registry(bool create) {
    mp_map_t *mods = &MP_STATE_VM(mp_loaded_modules_dict).map;
    mp_map_elem_t *e = mp_map_lookup(mods, MP_OBJ_NEW_QSTR(MP_QSTR__ccbor_ext), MP_MAP_LOOKUP);
    if (e != NULL && mp_obj_is_type(e->value, &mp_type_module)) {
        return e->value;
    }
    if (!create) {
        return MP_OBJ_NULL;
    }
    if (e != NULL) {
        mp_map_lookup(mods, MP_OBJ_NEW_QSTR(MP_QSTR__ccbor_ext), MP_MAP_LOOKUP_REMOVE_IF_FOUND);
    }
    mp_obj_t mod = mp_obj_new_module(MP_QSTR__ccbor_ext);
    mp_obj_t globals = MP_OBJ_FROM_PTR(mp_obj_module_get_globals(mod));
    mp_obj_dict_store(globals, MP_OBJ_NEW_QSTR(MP_QSTR_custom), mp_obj_new_dict(0));
    mp_obj_dict_store(globals, MP_OBJ_NEW_QSTR(MP_QSTR_builtins), mp_obj_new_dict(0));
    mp_obj_dict_store(globals, MP_OBJ_NEW_QSTR(MP_QSTR_tags), mp_obj_new_dict(0));
    return mod;
}

static mp_obj_t ext_dict(mp_obj_t reg, qstr name) {
    mp_obj_t globals = MP_OBJ_FROM_PTR(mp_obj_module_get_globals(reg));
    mp_obj_t d = mp_obj_dict_get(globals, MP_OBJ_NEW_QSTR(name));
    if (!mp_obj_is_dict_or_ordereddict(d)) {
        mp_raise_TypeError(MP_ERROR_TEXT("_ccbor_ext registry damaged"));
    }
    return d;
}

// The map of a registry dict, or NULL if there is no registry or it is empty
// (the common case, which then costs nothing per packed object).
static mp_map_t *ext_map(qstr name) {
    mp_obj_t reg = ext_registry(false);
    if (reg == MP_OBJ_NULL) {
        return NULL;
    }
    mp_map_t *map = mp_obj_dict_get_map(ext_dict(reg, name));
    return map->used ? map : NULL;
}

// The dicts built from **options, handed to the ext packb/unpackb methods.
static mp_obj_t options_dict(mp_obj_t *options, mp_map_t *kw) {
    if (*options == MP_OBJ_NULL) {
        *options = mp_obj_new_dict(kw == NULL ? 0 : kw->used);
        if (kw != NULL) {
            for (size_t i = 0; i < kw->alloc; i++) {
                if (mp_map_slot_is_filled(kw, i)) {
                    mp_obj_dict_store(*options, kw->table[i].key, kw->table[i].value);
                }
            }
        }
    }
    return *options;
}

static mp_obj_t kw_get(mp_map_t *kw, qstr name) {
    if (kw == NULL || kw->used == 0) {
        return MP_OBJ_NULL;
    }
    mp_map_elem_t *e = mp_map_lookup(kw, MP_OBJ_NEW_QSTR(name), MP_MAP_LOOKUP);
    return e == NULL ? MP_OBJ_NULL : e->value;
}

static bool kw_flag(mp_map_t *kw, qstr name) {
    mp_obj_t v = kw_get(kw, name);
    return v != MP_OBJ_NULL && mp_obj_is_true(v);
}

/******************************************************************************/
// The Tag type (D2).
//
// A native type compatible with moat's Tag (Tag(tag, value=None), .tag,
// .value, __eq__, __hash__, __repr__), and with cbor2's CBORTag. Unknown or
// unregistered tag numbers decode to it; packing it emits the tag number
// (mt6) followed by the packed value. Registered classes are dispatched by
// isinstance, so a Tag instance is ALWAYS packed generically.

typedef struct _mp_obj_tag_t {
    mp_obj_base_t base;
    mp_obj_t tag;    // an int, 0..2**64-1
    mp_obj_t value;  // any object; None as the constructor default
} mp_obj_tag_t;

extern const mp_obj_type_t ccbor_tag_type;


static mp_obj_t tag_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 2, false);
    if (!mp_obj_is_int(args[0])) {
        mp_raise_TypeError(MP_ERROR_TEXT("Tag number is not type integer"));
    }
    // Range check: 0 <= tag <= 2**64-1. On longlong builds a Python int
    // cannot even hold the top half, so the check reduces to >= 0 there.
    if (mp_obj_int_sign(args[0]) < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("Tag number is negative"));
    }
    #if MICROPY_LONGINT_IMPL == MICROPY_LONGINT_IMPL_MPZ
    if (!mp_obj_is_small_int(args[0])) {
        const mpz_t *z = &((mp_obj_int_t *)MP_OBJ_TO_PTR(args[0]))->mpz;
        if (z->len * MPZ_DIG_SIZE > 64) {
            mp_raise_ValueError(MP_ERROR_TEXT("Tag number out of range"));
        }
    }
    #endif
    mp_obj_tag_t *o = mp_obj_malloc(mp_obj_tag_t, type);
    o->tag = args[0];
    o->value = n_args > 1 ? args[1] : mp_const_none;
    return MP_OBJ_FROM_PTR(o);
}

static void tag_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    mp_obj_tag_t *self = MP_OBJ_TO_PTR(self_in);
    (void)kind;
    mp_print_str(print, "Tag(");
    mp_obj_print_helper(print, self->tag, PRINT_REPR);
    mp_print_str(print, ", ");
    mp_obj_print_helper(print, self->value, PRINT_REPR);
    mp_print_str(print, ")");
}

static void tag_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mp_obj_tag_t *self = MP_OBJ_TO_PTR(self_in);
    if (attr == MP_QSTR_tag) {
        dest[0] = self->tag;
    } else if (attr == MP_QSTR_value) {
        dest[0] = self->value;
    }
}

static mp_obj_t tag_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    mp_obj_tag_t *self = MP_OBJ_TO_PTR(self_in);
    switch (op) {
        case MP_UNARY_OP_BOOL:
            return mp_const_true;
        case MP_UNARY_OP_HASH: {
            // moat: hash(tag) ^ hash(value); a value that cannot be
            // hashed raises TypeError from mp_unary_op itself.
            mp_obj_t ht = mp_unary_op(MP_UNARY_OP_HASH, self->tag);
            mp_obj_t hv = mp_unary_op(MP_UNARY_OP_HASH, self->value);
            return MP_OBJ_NEW_SMALL_INT(MP_OBJ_SMALL_INT_VALUE(ht) ^ MP_OBJ_SMALL_INT_VALUE(hv));
        }
        default:
            return MP_OBJ_NULL;
    }
}

static mp_obj_t tag_binary_op(mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in) {
    if (op != MP_BINARY_OP_EQUAL) {
        return MP_OBJ_NULL;
    }
    if (!mp_obj_is_type(lhs_in, &ccbor_tag_type) || !mp_obj_is_type(rhs_in, &ccbor_tag_type)) {
        return mp_const_false;
    }
    mp_obj_tag_t *lhs = MP_OBJ_TO_PTR(lhs_in);
    mp_obj_tag_t *rhs = MP_OBJ_TO_PTR(rhs_in);
    return mp_obj_new_bool(
        mp_obj_equal(lhs->tag, rhs->tag) && mp_obj_equal(lhs->value, rhs->value));
}

MP_DEFINE_CONST_OBJ_TYPE(
    ccbor_tag_type,
    MP_QSTR_Tag,
    MP_TYPE_FLAG_NONE,
    make_new, tag_make_new,
    print, tag_print,
    attr, tag_attr,
    unary_op, tag_unary_op,
    binary_op, tag_binary_op
    );

/******************************************************************************/
// Packing

enum {
    FPREC_NATIVE,   // the build's own width, but the smallest exact one (D5)
    FPREC_HALF,
    FPREC_SINGLE,
    FPREC_DOUBLE,
    FPREC_INVALID,  // only an error when a float is actually packed
};

#if MICROPY_FLOAT_IMPL == MICROPY_FLOAT_IMPL_DOUBLE
#define CCBOR_FLOAT_PRECISION MP_QSTR_double
#else
#define CCBOR_FLOAT_PRECISION MP_QSTR_single
#endif

typedef struct _packer_t {
    vstr_t vstr;
    mp_map_t *kw;         // **options of dumps/dump
    mp_obj_t options;     // the same as a dict, built on first use
    mp_map_t *builtins;   // registered built-in tag types, NULL if none
    mp_map_t *custom;     // registered user classes, NULL if none
    uint16_t depth;
    uint8_t fprec;
    bool canonical;       // sort map keys like cbor2 (RFC 8949 4.2.3)
} packer_t;

static void pk_obj(packer_t *p, mp_obj_t o);

// The output buffer at least doubles when it grows, so packing is linear in
// the output size (vstr itself grows by a constant amount).
static MP_NOINLINE void pk_grow(vstr_t *vstr, size_t n) {
    size_t need = vstr->len + n;
    size_t alloc = vstr->alloc * 2;
    if (alloc < need) {
        alloc = need;
    }
    vstr->buf = m_renew(char, vstr->buf, vstr->alloc, alloc);
    vstr->alloc = alloc;
}

static inline byte *pk_reserve(packer_t *p, size_t n) {
    vstr_t *vstr = &p->vstr;
    if (vstr->alloc - vstr->len < n) {
        pk_grow(vstr, n);
    }
    byte *b = (byte *)vstr->buf + vstr->len;
    vstr->len += n;
    return b;
}

static inline void pk_byte(packer_t *p, byte b) {
    *pk_reserve(p, 1) = b;
}

static inline void put_be32(byte *b, uint32_t x, size_t n) {
    while (n--) {
        b[n] = (byte)x;
        x >>= 8;
    }
}

static void put_be64(byte *b, uint64_t x) {
    put_be32(b, (uint32_t)(x >> 32), 4);
    put_be32(b + 4, (uint32_t)x, 4);
}

// tag followed by n big-endian bytes of x, and room for extra payload bytes.
static byte *pk_hdr(packer_t *p, byte tag, uint32_t x, size_t n, size_t extra) {
    byte *b = pk_reserve(p, 1 + n + extra);
    b[0] = tag;
    put_be32(b + 1, x, n);
    return b + 1 + n;
}

static void pk_hdr64(packer_t *p, byte tag, uint64_t x) {
    byte *b = pk_reserve(p, 9);
    b[0] = tag;
    put_be64(b + 1, x);
}

// Major type mt (0..7) + argument x (0..2**64-1), then extra payload bytes.
// Preferred (minimal) additional information, as RFC 8949 4.2.1 requires.
static byte *pk_mt_hdr(packer_t *p, byte mt, uint64_t x, size_t extra) {
    if (x < 24) {
        byte *b = pk_reserve(p, 1 + extra);
        b[0] = mt | (byte)x;
        return b + 1;
    }
    if (x < 0x100) {
        return pk_hdr(p, mt | 24, (uint32_t)x, 1, extra);
    }
    if (x < 0x10000) {
        return pk_hdr(p, mt | 25, (uint32_t)x, 2, extra);
    }
    if (x <= 0xffffffff) {
        return pk_hdr(p, mt | 26, (uint32_t)x, 4, extra);
    }
    pk_hdr64(p, mt | 27, x);
    return pk_reserve(p, extra);
}

// mt0: 0 <= x <= 2**64-1.
static void pk_uint64(packer_t *p, uint64_t x) {
    pk_mt_hdr(p, 0x00, x, 0);
}

// mt1: -1 <= x <= -2**64, written as the argument n = -1 - x without
// materialising a 65-bit magnitude (so that -2**64 works: n = 2**64-1).
static void pk_negint64(packer_t *p, uint64_t n) {
    pk_mt_hdr(p, 0x20, n, 0);
}

// An int that fits in 64 bits, signed.
static void pk_int64(packer_t *p, int64_t x) {
    if (x >= 0) {
        pk_uint64(p, (uint64_t)x);
    } else {
        // -1 - x in uint64 arithmetic: x >= -2**63, so -1 - x <= 2**63 + ... < 2**64.
        uint64_t n = ~(uint64_t)x; // ~x == -1 - x modulo 2**64
        pk_negint64(p, n);
    }
}

// An int object that is not a small int.
static void pk_bigint(packer_t *p, mp_obj_t o) {
    #if MICROPY_LONGINT_IMPL == MICROPY_LONGINT_IMPL_MPZ
    // Read the mpz digits directly. mp_obj_int_to_bytes_impl() with an
    // overflow check is not enough: it lets -2**64 through as 0.
    const mpz_t *z = &((mp_obj_int_t *)MP_OBJ_TO_PTR(o))->mpz;
    // Bit length of the magnitude, and the low 64 bits. An mpz is
    // normalised (no leading zero limbs), so the top limb is nonzero.
    size_t nlimb = z->len;
    while (nlimb > 0 && z->dig[nlimb - 1] == 0) {
        nlimb--; // (defensive; a normalised mpz never needs this)
    }
    size_t topbits = 0;
    for (mp_uint_t probe = (mp_uint_t)1 << (MPZ_DIG_SIZE - 1); probe != 0; probe >>= 1) {
        if (z->dig[nlimb - 1] & probe) {
            break;
        }
        topbits++; // leading zeros of the top limb
    }
    size_t bits = nlimb * MPZ_DIG_SIZE - topbits; // bit length of |x|
    uint64_t mag = 0;
    for (size_t i = nlimb; i--;) {
        mag = (mag << MPZ_DIG_SIZE) | z->dig[i];
    }
    // mt0 covers 0 <= x <= 2**64-1 (bits <= 64);
    // mt1 covers -2**64 <= x <= -1, i.e. magnitude <= 2**64: bits <= 64,
    // or bits == 65 with only the top bit set (then the low 64 bits of
    // the magnitude are all zero).
    if (bits <= 64 || (z->neg && bits == 65 && mag == 0)) {
        if (z->neg) {
            // mt1 argument is -1 - x = mag - 1; for x == -2**64 it is
            // exactly 2**64-1 (all ones).
            uint64_t arg = (bits == 65) ? 0xffffffffffffffffULL : mag - 1;
            pk_negint64(p, arg);
        } else {
            pk_uint64(p, mag);
        }
        return;
    }
    // Bignum: tag 2 (x > 2**64-1) or tag 3 (x < -2**64) followed by the
    // big-endian MAGNITUDE as a byte string (RFC 8949 3.4.3). For tag 3
    // the magnitude is the one of -1-x, i.e. mag-1, computed limb by
    // limb with a borrow.
    //
    // The magnitude limbs go into a stack buffer; values this large are
    // rare, and 4 limbs of 32 bits (16 of 16 bits) cover 128 bits.
    // Larger values raise, like the other huge-object errors: encoding
    // them would need a heap buffer for the payload, and the byte-string
    // header (whose length depends on it) must be written first.
    if (nlimb > CCBOR_MAX_BIGNUM_LIMBS) {
        raise_unsupported_msg(z->neg ? MP_ERROR_TEXT("huge signed int") : MP_ERROR_TEXT("huge unsigned int"));
    }
    mpz_dig_t limbs[CCBOR_MAX_BIGNUM_LIMBS];
    mp_uint_t borrow = z->neg ? 1 : 0; // subtract 1 from the magnitude if negative
    for (size_t i = 0; i < nlimb; i++) {
        mp_uint_t v = z->dig[i];
        mp_uint_t nv = (v - borrow) & (((mp_uint_t)1 << MPZ_DIG_SIZE) - 1);
        borrow = (v == 0 && borrow != 0) ? 1 : 0;
        limbs[i] = (mpz_dig_t)nv;
    }
    // Skip leading zero limbs of the (possibly decremented) magnitude,
    // then count bytes: nbytes = ceil(bits(mag)/8).
    while (nlimb > 0 && limbs[nlimb - 1] == 0) {
        nlimb--;
    }
    if (nlimb == 0) {
        // The decrement emptied the magnitude: cannot happen here (the
        // -2**64 case returned above), but keep the smallest correct form.
        nlimb = 1;
        limbs[0] = 0;
    }
    topbits = 0;
    for (mp_uint_t probe = (mp_uint_t)1 << (MPZ_DIG_SIZE - 1); probe != 0; probe >>= 1) {
        if (limbs[nlimb - 1] & probe) {
            break;
        }
        topbits++;
    }
    bits = nlimb * MPZ_DIG_SIZE - topbits;
    size_t nbytes = (bits + 7) / 8;
    pk_mt_hdr(p, z->neg ? 0xc3 : 0xc2, 2, 0); // the tag head (number 2 or 3)
    byte *payload = pk_mt_hdr(p, 0x40, nbytes, nbytes);
    for (size_t i = 0; i < nbytes; i++) {
        size_t bitpos = (nbytes - 1 - i) * 8; // bit offset of this byte from the LSB
        size_t limb = bitpos / MPZ_DIG_SIZE;
        unsigned shift = bitpos % MPZ_DIG_SIZE;
        uint32_t val = 0;
        if (limb < nlimb) {
            val = (uint32_t)(limbs[limb] >> shift);
            if (shift + 8 > MPZ_DIG_SIZE && limb + 1 < nlimb) {
                val |= (uint32_t)limbs[limb + 1] << (MPZ_DIG_SIZE - shift);
            }
        }
        payload[i] = (byte)val;
    }
    #elif MICROPY_LONGINT_IMPL == MICROPY_LONGINT_IMPL_LONGLONG
    pk_int64(p, ((mp_obj_int_t *)MP_OBJ_TO_PTR(o))->val);
    #else
    (void)o;
    pk_int64(p, mp_obj_get_int(o));
    #endif
}

#if MICROPY_PY_BUILTINS_FLOAT
// IEEE 754 half-precision conversions. f16 <-> f32 bit manipulation
// without any library float support (works on every build).
static uint32_t f32_to_f16_bits(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t man = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff) {
        // inf / NaN: keep the top 10 mantissa bits.
        return sign | 0x7c00 | (man >> 13);
    }
    if (exp >= 0x1f) {
        // overflow -> inf (round-to-nearest-even of huge values)
        return sign | 0x7c00;
    }
    if (exp <= 0) {
        // subnormal or zero: round the full 24-bit mantissa into the
        // half subnormal range (10 bits starting at 2**-24).
        if (exp < -10) {
            return sign; // too small: rounds to zero (nearest-even)
        }
        uint32_t m = man | 0x800000; // implicit 1
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half = 1u << (shift - 1);
        uint32_t val = m >> shift;
        if ((m & half) && ((m & (half - 1)) || ((val & 1) != 0))) {
            val++;
        }
        return sign | val;
    }
    // Normal: round the 13 dropped mantissa bits to nearest-even.
    uint32_t val = (uint32_t)(exp << 10) | (man >> 13);
    uint32_t rem = man & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && ((man >> 13) & 1))) {
        val++;
        if (val & 0x7c00) { // mantissa overflowed into the exponent
            val = (uint32_t)(exp + 1) << 10;
        }
    }
    return sign | val;
}

static float f16_bits_to_f32(uint32_t h) {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t man = h & 0x3ff;
    uint32_t x;
    if (exp == 0) {
        if (man == 0) {
            x = sign;
        } else {
            // subnormal half -> normal single
            exp = 127 - 15 + 1;
            while ((man & 0x400) == 0) {
                man <<= 1;
                exp--;
            }
            man &= 0x3ff;
            x = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1f) {
        x = sign | 0xff << 23 | (man << 13);
    } else {
        x = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &x, sizeof(f));
    return f;
}

// The smallest float width that keeps the value exactly (D5), unless an
// explicit force_float_precision was given. NaN always becomes f97e00
// (its payload is not kept, like cbor2): it never compares equal, so it
// must be special-cased before the exactness tests.
static void pk_float(packer_t *p, mp_float_t f) {
    if (p->fprec == FPREC_NATIVE) {
        double d = (double)f; // mp_float_t is float or double
        uint64_t u64;
        memcpy(&u64, &d, sizeof(u64));
        if ((u64 & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL) {
            pk_hdr(p, 0xf9, 0x7e00, 2, 0); // NaN -> f97e00
            return;
        }
        // Exact as f16? (A value that overflows to inf in half precision
        // is NOT exact: compare the round trip against the original.)
        float f32 = (float)d;
        uint32_t u32;
        memcpy(&u32, &f32, sizeof(u32));
        uint32_t h = f32_to_f16_bits(f32);
        if ((double)f16_bits_to_f32(h) == d) {
            pk_hdr(p, 0xf9, h, 2, 0);
            return;
        }
        // Exact as f32?
        if ((double)f32 == d) {
            pk_hdr(p, 0xfa, u32, 4, 0);
            return;
        }
        pk_hdr64(p, 0xfb, u64);
    } else if (p->fprec == FPREC_HALF) {
        float s = (float)f;
        uint32_t u;
        memcpy(&u, &s, sizeof(u));
        pk_hdr(p, 0xf9, f32_to_f16_bits(s), 2, 0);
    } else if (p->fprec == FPREC_SINGLE) {
        float s = (float)f;
        uint32_t u;
        memcpy(&u, &s, sizeof(u));
        pk_hdr(p, 0xfa, u, 4, 0);
    } else if (p->fprec == FPREC_DOUBLE) {
        double d = (double)f;
        uint64_t u;
        memcpy(&u, &d, sizeof(u));
        pk_hdr64(p, 0xfb, u);
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid float precision"));
    }
}
#endif

static void pk_str(packer_t *p, const byte *s, size_t len) {
    byte *b = pk_mt_hdr(p, 0x60, len, len);
    memcpy(b, s, len);
}

static void pk_bin(packer_t *p, mp_obj_t o) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(o, &bi, MP_BUFFER_READ);
    byte *b = pk_mt_hdr(p, 0x40, bi.len, bi.len);
    memcpy(b, bi.buf, bi.len);
}

// Array (mt4) and map (mt5) headers.
static void pk_container_hdr(packer_t *p, byte mt, size_t len) {
    pk_mt_hdr(p, mt, len, 0);
}

static void pk_enter(packer_t *p) {
    MP_STACK_CHECK();
    if (++p->depth > CCBOR_MAX_DEPTH) {
        raise_depth();
    }
}

static void pk_tuple(packer_t *p, mp_obj_tuple_t *t) {
    pk_container_hdr(p, 0x80, t->len);
    pk_enter(p);
    for (size_t i = 0; i < t->len; i++) {
        pk_obj(p, t->items[i]);
    }
    p->depth--;
}

// The list may change under us only if a tag packb (Python code) changes it.
// Items are re-read on every iteration and a size change is an error: the
// header already written would no longer match.
static void pk_list(packer_t *p, mp_obj_list_t *l) {
    size_t n = l->len;
    pk_container_hdr(p, 0x80, n);
    pk_enter(p);
    for (size_t i = 0; i < n; i++) {
        if (l->len != n) {
            raise_changed();
        }
        pk_obj(p, l->items[i]);
    }
    if (l->len != n) {
        raise_changed();
    }
    p->depth--;
}

// Same order as `for k, v in d.items()`: the slots of the table in order.
// With canonical=True the entries are sorted first, like cbor2 does: by
// (length of the encoded key, then the encoded key bytes) - RFC 8949
static void pk_map(packer_t *p, mp_obj_t d) {
    mp_map_t *map = mp_obj_dict_get_map(d);
    size_t n = map->used;
    // Collect the (key, value) pairs into a rooted list first: sorting
    // reorders them, and a raw m_new() array would not be visible to the
    // GC if Python code runs during key packing (a registered tag class
    // used as a key, for instance). A list object is safe, and doubles
    // as the buffer for the insertion sort.
    mp_obj_t kv = mp_obj_new_list(2 * n, NULL);
    mp_obj_t *items = ((mp_obj_list_t *)MP_OBJ_TO_PTR(kv))->items;
    size_t done = 0;
    for (size_t i = 0; i < map->alloc; i++) {
        if (mp_map_slot_is_filled(map, i)) {
            if (done == n) {
                raise_changed();
            }
            items[2 * done] = map->table[i].key;
            items[2 * done + 1] = map->table[i].value;
            done++;
        }
    }
    if (done != n || map->used != n) {
        raise_changed();
    }
    if (p->canonical && n > 1) {
        // Encode every key into a scratch buffer, sort the entry indices
        // by (encoded length, encoded bytes) - RFC 8949 4.2.3's
        // "length-first" order, which is what cbor2 6.1.2 implements
        // (verified against it: -1 sorts before 24) - then emit. The
        // scratch packer shares the options but has its own buffer.
        packer_t kp;
        vstr_init(&kp.vstr, 32);
        kp.kw = p->kw;
        kp.options = MP_OBJ_NULL;
        kp.builtins = p->builtins;
        kp.custom = p->custom;
        kp.depth = 0;
        kp.fprec = p->fprec;
        kp.canonical = true;
        // Each key's encoded range travels with its entry: keypos[i] is
        // the start and keylen[i] the length of the bytes of the key now
        // at position i. (Deriving lengths from consecutive offsets would
        // break as soon as the order is permuted.)
        size_t *keypos = m_new(size_t, n);
        size_t *keylen = m_new(size_t, n);
        for (size_t i = 0; i < n; i++) {
            keypos[i] = kp.vstr.len;
            pk_obj(&kp, items[2 * i]);
            keylen[i] = kp.vstr.len - keypos[i];
        }
        // Insertion sort of the entries by (key length, key bytes): maps
        // are usually small, and this needs no callback context.
        for (size_t i = 1; i < n; i++) {
            size_t o = keypos[i];
            size_t olen = keylen[i];
            mp_obj_t k = items[2 * i];
            mp_obj_t v = items[2 * i + 1];
            size_t j = i;
            while (j > 0) {
                size_t plen = keylen[j - 1];
                const byte *a = (const byte *)kp.vstr.buf + keypos[j - 1];
                const byte *b = (const byte *)kp.vstr.buf + o;
                int cmp;
                if (plen != olen) {
                    cmp = plen < olen ? -1 : 1;
                } else {
                    cmp = memcmp(a, b, plen);
                }
                if (cmp <= 0) {
                    break;
                }
                // shift the entry at j-1 right to j
                keypos[j] = keypos[j - 1];
                keylen[j] = keylen[j - 1];
                items[2 * j] = items[2 * (j - 1)];
                items[2 * j + 1] = items[2 * (j - 1) + 1];
                j--;
            }
            keypos[j] = o;
            keylen[j] = olen;
            items[2 * j] = k;
            items[2 * j + 1] = v;
        }
        m_del(size_t, keypos, n);
        m_del(size_t, keylen, n);
        vstr_clear(&kp.vstr);
    }
    pk_container_hdr(p, 0xa0, n);
    pk_enter(p);
    for (size_t i = 0; i < n; i++) {
        pk_obj(p, items[2 * i]);
        pk_obj(p, items[2 * i + 1]);
        // A tag packb (Python) may have run: the map must still have
        // exactly n entries, or the header already written is wrong.
        if (map->used != n) {
            raise_changed();
        }
    }
    p->depth--;
}

static inline bool is_instance(mp_obj_t o, mp_obj_t type) {
    return mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(o)), type);
}

// Tag number of a Tag object, as uint64. The Python-visible .tag is an
// int; values above 2**63-1 only exist on builds with bigger ints (on
// longlong builds the constructor rejects them).
// A tag number given as a Python int, as uint64 (0..2**64-1). Rejects
// negatives; on longlong builds values above 2**63-1 cannot exist.
static uint64_t tag_number(mp_obj_t t) {
    if (mp_obj_is_small_int(t)) {
        mp_int_t v = MP_OBJ_SMALL_INT_VALUE(t);
        if (v < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("Tag number is negative"));
        }
        return (uint64_t)v;
    }
    #if MICROPY_LONGINT_IMPL == MICROPY_LONGINT_IMPL_MPZ
    {
        const mpz_t *z = &((mp_obj_int_t *)MP_OBJ_TO_PTR(t))->mpz;
        if (z->neg || z->len * MPZ_DIG_SIZE > 64) {
            mp_raise_ValueError(MP_ERROR_TEXT("Tag number out of range"));
        }
        uint64_t u = 0;
        for (size_t i = z->len; i--;) {
            u = (u << MPZ_DIG_SIZE) | z->dig[i];
        }
        return u;
    }
    #else
    mp_raise_ValueError(MP_ERROR_TEXT("Tag number out of range"));
    #endif
}

// Tag header (mt6) followed by the packed content: a Tag instance packs
// generically (D2), a registered class's packb() result packs as the
// content (D1: the content is a CBOR item, not raw bytes).
static void pk_tag(packer_t *p, mp_obj_t tag_num, mp_obj_t content) {
    uint64_t n = tag_number(tag_num);
    // Tags count towards the depth limit (review point 8): a packb()
    // that returns its own class, or nested Tags, must not recurse for
    // ever, and a chain of tags costs C stack per level.
    pk_enter(p);
    pk_mt_hdr(p, 0xc0, n, 0);
    pk_obj(p, content);
    p->depth--;
}

// A registered built-in type (e.g. complex, set, tuple) has priority over
// the native encodings, as in umsgpack. Returns false if o has no tag
// packer. The packb() result is packed as the tag CONTENT (D1).
static bool pk_builtin_ext(packer_t *p, mp_obj_t o) {
    mp_map_t *map = p->builtins;
    for (size_t i = 0; i < map->alloc; i++) {
        if (!mp_map_slot_is_filled(map, i) || !is_instance(o, map->table[i].key)) {
            continue;
        }
        // value is (packer class, tag number)
        size_t n;
        mp_obj_t *pair;
        mp_obj_tuple_get(map->table[i].value, &n, &pair);
        mp_obj_t tag_num = pair[1];
        mp_obj_t dest[4];
        mp_load_method_maybe(pair[0], MP_QSTR_packb, dest);
        if (dest[0] == MP_OBJ_NULL) {
            raise_with_obj(&mp_type_NotImplementedError, "Class ",
                MP_OBJ_FROM_PTR(mp_obj_get_type(o)), PRINT_REPR, " invalid packb()");
        }
        dest[2] = o;
        dest[3] = options_dict(&p->options, p->kw);
        pk_tag(p, tag_num, mp_call_method_n_kw(2, 0, dest));
        return true;
    }
    return false;
}

// Last chance: an instance of a class registered with @tag_serializable(n).
static void pk_custom_ext(packer_t *p, mp_obj_t o) {
    mp_map_t *map = p->custom;
    for (size_t i = 0; map != NULL && i < map->alloc; i++) {
        if (!mp_map_slot_is_filled(map, i) || !is_instance(o, map->table[i].key)) {
            continue;
        }
        mp_obj_t tag_num = map->table[i].value;
        mp_obj_t dest[2];
        mp_load_method_maybe(o, MP_QSTR_packb, dest);
        if (dest[0] == MP_OBJ_NULL) {
            break;
        }
        pk_tag(p, tag_num, mp_call_method_n_kw(0, 0, dest));
        return;
    }
    raise_unsupported_type(o);
}

// Instances of subclasses of the native types (and native subtypes such as
// namedtuple), in the order umsgpack tests them with isinstance().
static void pk_subclass(packer_t *p, mp_obj_t o, const mp_obj_type_t *type) {
    static const mp_obj_type_t *const natives[] = {
        &mp_type_int,
        #if MICROPY_PY_BUILTINS_FLOAT
        &mp_type_float,
        #endif
        &mp_type_str, &mp_type_bytes,
        #if MICROPY_PY_BUILTINS_BYTEARRAY
        &mp_type_bytearray,
        #endif
        &mp_type_list, &mp_type_tuple, &mp_type_dict,
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(natives); i++) {
        if (!mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(type), MP_OBJ_FROM_PTR(natives[i]))) {
            continue;
        }
        if (mp_obj_is_instance_type(type)) {
            // A Python subclass: pack the native object it wraps.
            pk_obj(p, mp_obj_cast_to_native_base(o, MP_OBJ_FROM_PTR(natives[i])));
            return;
        }
        if (natives[i] == &mp_type_tuple) {
            // A native subtype with the tuple layout (namedtuple).
            pk_tuple(p, MP_OBJ_TO_PTR(o));
            return;
        }
        // Other native subtypes have an unknown layout: not packed natively.
        break;
    }
    pk_custom_ext(p, o);
}

static void pk_obj(packer_t *p, mp_obj_t o) {
    if (o == mp_const_none) {
        pk_byte(p, 0xf6);
        return;
    }
    if (o == MP_OBJ_FROM_PTR(&mp_const_ellipsis_obj)) {
        pk_byte(p, 0xf7);
        return;
    }
    if (p->builtins != NULL && pk_builtin_ext(p, o)) {
        return;
    }
    if (mp_obj_is_small_int(o)) {
        pk_int64(p, MP_OBJ_SMALL_INT_VALUE(o));
    } else if (mp_obj_is_qstr(o)) {
        size_t len;
        const byte *s = qstr_data(MP_OBJ_QSTR_VALUE(o), &len);
        pk_str(p, s, len);
    } else if (o == mp_const_false) {
        pk_byte(p, 0xf4);
    } else if (o == mp_const_true) {
        pk_byte(p, 0xf5);
        #if MICROPY_PY_BUILTINS_FLOAT
    } else if (mp_obj_is_float(o)) {
        pk_float(p, mp_obj_float_get(o));
        #endif
    } else {
        const mp_obj_type_t *type = mp_obj_get_type(o);
        if (type == &mp_type_str) {
            GET_STR_DATA_LEN(o, s, len);
            pk_str(p, s, len);
        } else if (type == &ccbor_tag_type) {
            // A Tag instance packs generically (D2): number + content.
            mp_obj_tag_t *t = MP_OBJ_TO_PTR(o);
            pk_tag(p, t->tag, t->value);
        } else if (type == &mp_type_dict
                   #if MICROPY_PY_COLLECTIONS_ORDEREDDICT
                   || type == &mp_type_ordereddict
                   #endif
                   ) {
            pk_map(p, o);
        } else if (type == &mp_type_list) {
            pk_list(p, MP_OBJ_TO_PTR(o));
        } else if (type == &mp_type_tuple) {
            pk_tuple(p, MP_OBJ_TO_PTR(o));
        } else if (type == &mp_type_int) {
            pk_bigint(p, o);
        } else if (type == &mp_type_bytes
                   #if MICROPY_PY_BUILTINS_BYTEARRAY
                   || type == &mp_type_bytearray
                   #endif
                   #if MICROPY_PY_BUILTINS_MEMORYVIEW
                   || type == &mp_type_memoryview
                   #endif
                   ) {
            pk_bin(p, o);
        } else {
            pk_subclass(p, o, type);
        }
    }
}

static void packer_init(packer_t *p, mp_map_t *kw) {
    vstr_init(&p->vstr, 64);
    p->kw = kw;
    p->options = MP_OBJ_NULL;
    p->depth = 0;
    p->fprec = FPREC_NATIVE;
    p->canonical = kw_flag(kw, MP_QSTR_canonical);
    mp_obj_t prec = kw_get(kw, MP_QSTR_force_float_precision);
    if (prec != MP_OBJ_NULL) {
        if (mp_obj_equal(prec, MP_OBJ_NEW_QSTR(MP_QSTR_double))) {
            p->fprec = FPREC_DOUBLE;
        } else if (mp_obj_equal(prec, MP_OBJ_NEW_QSTR(MP_QSTR_single))) {
            p->fprec = FPREC_SINGLE;
        } else if (mp_obj_equal(prec, MP_OBJ_NEW_QSTR(MP_QSTR_half))) {
            p->fprec = FPREC_HALF;
        } else {
            // Like umsgpack, only an error if a float is actually packed.
            p->fprec = FPREC_INVALID;
        }
    }
    p->builtins = ext_map(MP_QSTR_builtins);
    p->custom = ext_map(MP_QSTR_custom);
}

// dumps(obj, **options) -> bytes
static mp_obj_t ccbor_dumps(size_t n_args, const mp_obj_t *args, mp_map_t *kw) {
    mp_arg_check_num(n_args, kw->used, 1, 1, true);
    packer_t p;
    packer_init(&p, kw);
    pk_obj(&p, args[0]);
    return mp_obj_new_bytes_from_vstr(&p.vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ccbor_dumps_obj, 1, ccbor_dumps);

// Look up fp.<meth> into dest. If it is the generic C stream method, fp is a
// native stream: return its protocol so data can be moved without method
// calls. Otherwise (Python file-like objects) return NULL and use dest.
// The protocol slot alone is not enough: other types use it for other
// protocols (machine.Pin, for instance).
static const mp_stream_p_t *native_stream(mp_obj_t fp, qstr meth, const void *generic, mp_obj_t *dest) {
    mp_load_method(fp, meth, dest);
    if (dest[0] == MP_OBJ_FROM_PTR(generic)) {
        return MP_OBJ_TYPE_GET_SLOT_OR_NULL(mp_obj_get_type(fp), protocol);
    }
    return NULL;
}

// dump(obj, fp, **options): pack, then write everything with a single write.
static mp_obj_t ccbor_dump(size_t n_args, const mp_obj_t *args, mp_map_t *kw) {
    mp_arg_check_num(n_args, kw->used, 2, 2, true);
    packer_t p;
    packer_init(&p, kw);
    pk_obj(&p, args[0]);
    mp_obj_t fp = args[1];
    mp_obj_t dest[3];
    const mp_stream_p_t *sp = native_stream(fp, MP_QSTR_write, &mp_stream_write_obj, dest);
    if (sp != NULL && sp->write != NULL) {
        int err;
        mp_uint_t n = mp_stream_rw(fp, p.vstr.buf, p.vstr.len, &err, MP_STREAM_RW_WRITE);
        if (err != 0) {
            mp_raise_OSError(err);
        }
        if (n != p.vstr.len) {
            mp_raise_OSError(MP_EIO);
        }
        vstr_clear(&p.vstr);
    } else {
        dest[2] = mp_obj_new_bytes_from_vstr(&p.vstr);
        mp_call_method_n_kw(1, 0, dest);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ccbor_dump_obj, 2, ccbor_dump);

/******************************************************************************/
// Unpacking

typedef struct _unpacker_t {
    // Buffer mode: [cur, end) is the data still to decode.
    const byte *cur;
    const byte *end;
    // Set for a mutable source (bytearray, memoryview): Python code run by an
    // ext unpackb() could resize it, so the pointers are re-validated after.
    mp_obj_t src;
    const byte *base;
    // The object the buffer belongs to (loads: the input). Only set when it
    // is decoded from: the memory_min option returns views into it.
    mp_obj_t view_src;
    // Stream mode (load): stream != MP_OBJ_NULL.
    mp_obj_t stream;
    const mp_stream_p_t *sp; // NULL: use the read method instead
    mp_obj_t read_meth[3];
    mp_map_t *tags;          // registered tag types, NULL if none
    mp_map_t *kw;            // **options, for the ext unpackb methods
    mp_obj_t options;
    uint16_t depth;
    bool use_tuple;
    bool use_ordered_dict;
    bool allow_invalid_utf8;
    bool key; // the object being decoded is a map key
    uint16_t indef; // open indefinite containers (BREAK is valid if > 0)
    size_t memory_min; // byte strings at least this long decode as views
    byte hdr[8];
} unpacker_t;

static mp_obj_t up_obj(unpacker_t *u);

static void up_stream_read(unpacker_t *u, byte *buf, size_t n) {
    if (u->sp != NULL) {
        int err;
        mp_uint_t got = mp_stream_rw(u->stream, buf, n, &err, MP_STREAM_RW_READ);
        if (err != 0) {
            mp_raise_OSError(err);
        }
        if (got != n) {
            raise_insufficient();
        }
        return;
    }
    // Any object with a read(n) method, called until it has supplied n bytes.
    while (n > 0) {
        u->read_meth[2] = MP_OBJ_NEW_SMALL_INT(n);
        mp_obj_t data = mp_call_method_n_kw(1, 0, u->read_meth);
        if (data == mp_const_none) {
            raise_insufficient();
        }
        mp_buffer_info_t bi;
        mp_get_buffer_raise(data, &bi, MP_BUFFER_READ);
        if (bi.len == 0) {
            raise_insufficient();
        }
        if (bi.len > n) {
            mp_raise_ValueError(MP_ERROR_TEXT("read() returned too much data"));
        }
        memcpy(buf, bi.buf, bi.len);
        buf += bi.len;
        n -= bi.len;
    }
}

// n (<= 8) header bytes.
static inline const byte *up_read(unpacker_t *u, size_t n) {
    if (u->stream == MP_OBJ_NULL) {
        if ((size_t)(u->end - u->cur) < n) {
            raise_insufficient();
        }
        const byte *b = u->cur;
        u->cur += n;
        return b;
    }
    up_stream_read(u, u->hdr, n);
    return u->hdr;
}

static uint32_t up_be(unpacker_t *u, size_t n) {
    const byte *b = up_read(u, n);
    uint32_t x = 0;
    while (n--) {
        x = (x << 8) | *b++;
    }
    return x;
}

static uint64_t up_be64(unpacker_t *u) {
    const byte *b = up_read(u, 8);
    uint64_t x = 0;
    for (size_t i = 0; i < 8; i++) {
        x = (x << 8) | b[i];
    }
    return x;
}

// n payload bytes. Buffer mode: a pointer into the input. Stream mode: read
// into *vstr (initialised here), growing it as data arrives so a corrupt length
// cannot allocate more memory than the stream really delivers.
static const byte *up_payload(unpacker_t *u, size_t n, vstr_t *vstr) {
    if (u->stream == MP_OBJ_NULL) {
        if ((size_t)(u->end - u->cur) < n) {
            raise_insufficient();
        }
        const byte *b = u->cur;
        u->cur += n;
        return b;
    }
    vstr_init(vstr, n < 256 ? n : 256);
    while (vstr->len < n) {
        size_t chunk = n - vstr->len;
        size_t limit = vstr->len < 256 ? 256 : vstr->len;
        if (chunk > limit) {
            chunk = limit;
        }
        byte *b = (byte *)vstr_add_len(vstr, chunk);
        up_stream_read(u, b, chunk);
    }
    return (const byte *)vstr->buf;
}

static mp_obj_t up_new_bytes(unpacker_t *u, const byte *b, size_t n, vstr_t *vstr) {
    if (u->stream == MP_OBJ_NULL) {
        return mp_obj_new_bytes(b, n);
    }
    return mp_obj_new_bytes_from_vstr(vstr);
}

#if MICROPY_PY_BUILTINS_MEMORYVIEW
// A byte string of n bytes at b, as a memoryview into the caller's buffer
// instead of a copy (the memory_min option, buffer mode only). The layout is
// the memoryview constructor's: items points at the start of the backing
// data and memview_offset carries the payload offset, in bytes, so the GC
// finds the backing block through items (an interior pointer would not be
// traced). A memoryview input contributes its own items and offset, so a
// view of a view keeps the original buffer reachable.
static mp_obj_t up_new_view(unpacker_t *u, const byte *b, size_t n) {
    mp_obj_array_t *mv = mp_obj_malloc(mp_obj_array_t, &mp_type_memoryview);
    byte typecode = 'B';
    mp_obj_t items;
    size_t off;
    if (mp_obj_get_type(u->view_src) == &mp_type_memoryview) {
        // A view of a view: keep the original buffer reachable (its items is
        // what the GC traces) and accumulate the offsets, in bytes. The
        // offset lives in the 'free' bitfield (cf. objarray.h).
        mp_obj_array_t *src = MP_OBJ_TO_PTR(u->view_src);
        size_t item_sz = mp_binary_get_size('@', src->typecode & 0x7f, NULL);
        off = src->free * item_sz + (size_t)(b - u->base);
        items = src->items;
        if (src->typecode & MP_OBJ_ARRAY_TYPECODE_FLAG_RW) {
            typecode |= MP_OBJ_ARRAY_TYPECODE_FLAG_RW;
        }
    } else {
        mp_buffer_info_t bi;
        mp_get_buffer_raise(u->view_src, &bi, MP_BUFFER_READ);
        off = (size_t)(b - u->base);
        items = bi.buf;
        if (mp_get_buffer(u->view_src, &bi, MP_BUFFER_RW)) {
            typecode |= MP_OBJ_ARRAY_TYPECODE_FLAG_RW;
        }
    }
    if (off > ((1LL << MP_OBJ_ARRAY_FREE_SIZE_BITS) - 1)) {
        mp_raise_msg(&mp_type_OverflowError, MP_ERROR_TEXT("memoryview offset too large"));
    }
    mp_obj_memoryview_init(mv, typecode, off, n, items);
    return MP_OBJ_FROM_PTR(mv);
}
#endif

static mp_obj_t up_bin(unpacker_t *u, size_t n, bool key) {
    #if MICROPY_PY_BUILTINS_MEMORYVIEW
    // A view is unhashable, so map keys always copy. Only loads() has a
    // caller-owned buffer (view_src) to refer to; up_payload does not touch
    // the vstr in buffer mode, so NULL is fine here.
    if (u->memory_min != 0 && !key && u->view_src != MP_OBJ_NULL && n >= u->memory_min) {
        const byte *b = up_payload(u, n, NULL);
        return up_new_view(u, b, n);
    }
    #else
    (void)key;
    #endif
    vstr_t vstr;
    const byte *b = up_payload(u, n, &vstr);
    return up_new_bytes(u, b, n, &vstr);
}

static mp_obj_t up_str(unpacker_t *u, size_t n, bool key) {
    vstr_t vstr;
    const byte *s = up_payload(u, n, &vstr);
    #if MICROPY_PY_BUILTINS_STR_UNICODE && MICROPY_PY_BUILTINS_STR_UNICODE_CHECK
    if (!unicode_encoding_check(MP_ENCODING_UTF8, s, n)) {
        if (u->allow_invalid_utf8) {
            return up_new_bytes(u, s, n, &vstr);
        }
        mp_raise_msg(&mp_type_ccbor_InvalidStringException, MP_ERROR_TEXT("unpacked string is invalid utf-8"));
    }
    #endif
    #if CCBOR_REUSE_QSTR
    // A key that is an existing qstr: no allocation, and dict lookups compare
    // qstrs. New qstrs are never created from input data.
    qstr q = key ? qstr_find_strn((const char *)s, n) : MP_QSTRnull;
    if (q != MP_QSTRnull) {
        if (u->stream != MP_OBJ_NULL) {
            vstr_clear(&vstr);
        }
        return MP_OBJ_NEW_QSTR(q);
    }
    #else
    (void)key;
    #endif
    if (u->stream != MP_OBJ_NULL) {
        return mp_obj_new_str_from_vstr(&vstr);
    }
    return mp_obj_new_str_copy(&mp_type_str, s, n);
}

// Python code (an ext unpackb, a __hash__ or __eq__ of a key) has run: if the
// source buffer is mutable it may have been resized or moved.
static void up_revalidate(unpacker_t *u) {
    if (u->src == MP_OBJ_NULL) {
        return;
    }
    mp_buffer_info_t bi;
    mp_get_buffer_raise(u->src, &bi, MP_BUFFER_READ);
    size_t cur = u->cur - u->base;
    size_t end = u->end - u->base;
    if (bi.len < end) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("buffer changed during unpacking"));
    }
    u->base = bi.buf;
    u->cur = u->base + cur;
    u->end = u->base + end;
}

// An int from up to 8 big-endian bytes: 0..2**64-1. On builds without
// big ints (MICROPY_LONGINT_IMPL_NONE) values above the small-int range
// raise (the same way as msgpackc's int64 paths did); on longlong builds
// 2**63..2**64-1 cannot be an int and are wrapped in Tag by the caller
// (D4); on mpz builds they become big ints.
static mp_obj_t up_uint64_obj(uint64_t x) {
    if (x <= (uint64_t)MP_SMALL_INT_MAX) {
        return MP_OBJ_NEW_SMALL_INT((mp_int_t)x);
    }
    #if MICROPY_LONGINT_IMPL == MICROPY_LONGINT_IMPL_MPZ
    return mp_obj_new_int_from_ull(x);
    #elif MICROPY_LONGINT_IMPL == MICROPY_LONGINT_IMPL_LONGLONG
    if (x <= 0x7fffffffffffffffULL) {
        return mp_obj_new_int_from_ll((long long)x);
    }
    return MP_OBJ_NULL; // caller wraps in Tag (D4)
    #else
    raise_unsupported_msg(MP_ERROR_TEXT("int too large for this build"));
    #endif
}

// -1-n for an mt1 argument n: the result is -1..-2**64. Small ints cover
// -1..-2**30ish on every build; on longlong builds values below LONG_LONG_MIN
// cannot be an int and the caller wraps the ORIGINAL argument in Tag(3, ...)
// (D4); on mpz builds the bignum path handles everything.
static mp_obj_t up_negint_obj(uint64_t n) {
    if (n <= (uint64_t)MP_SMALL_INT_MAX) {
        return MP_OBJ_NEW_SMALL_INT(-1 - (mp_int_t)n);
    }
    #if MICROPY_LONGINT_IMPL == MICROPY_LONGINT_IMPL_LONGLONG
    if (n < 0x8000000000000000ULL) {
        return mp_obj_new_int_from_ll(-1 - (long long)n);
    }
    return MP_OBJ_NULL; // caller wraps in Tag (D4)
    #elif MICROPY_LONGINT_IMPL == MICROPY_LONGINT_IMPL_MPZ
    if (n < 0x8000000000000000ULL) {
        return mp_obj_new_int_from_ll(-1 - (long long)n);
    }
    // The value is -1-n, i.e. the NEGATIVE of the magnitude (n+1), which
    // is 2**63..2**64 here. n+1 overflows uint64 only when n == 2**64-1
    // (then the magnitude is exactly 2**64: 01 00..00).
    byte mag[9];
    mag[0] = n == 0xffffffffffffffffULL ? 1 : 0;
    put_be64(mag + 1, n + 1); // wraps only in the mag[0] == 1 case
    return mp_unary_op(MP_UNARY_OP_NEGATIVE,
        mp_obj_int_from_bytes_impl(true, 9, mag));
    #else
    (void)n;
    raise_unsupported_msg(MP_ERROR_TEXT("int too large for this build"));
    #endif
}

// The content of a bignum tag (2/3): a byte string holding the big-endian
// magnitude. On mpz builds it becomes an int (leading zero bytes accepted,
// like cbor2); on longlong builds an out-of-64-bit value is wrapped in
// Tag(2/3, bytes) (D4): no exception for well-formed CBOR.
static mp_obj_t up_bignum(unpacker_t *u, size_t n, bool negative) {
    vstr_t vstr;
    const byte *b = up_payload(u, n, &vstr);
    // Trim leading zeros (non-minimal but valid input).
    while (n > 0 && b[0] == 0) {
        b++;
        n--;
    }
    #if MICROPY_LONGINT_IMPL == MICROPY_LONGINT_IMPL_MPZ
    mp_obj_t mag = n == 0 ? MP_OBJ_NEW_SMALL_INT(0) : mp_obj_int_from_bytes_impl(true, n, b);
    if (u->stream != MP_OBJ_NULL) {
        vstr_clear(&vstr);
    }
    if (negative) {
        return mp_binary_op(MP_BINARY_OP_SUBTRACT, MP_OBJ_NEW_SMALL_INT(-1), mag);
    }
    return mag;
    #else
    // longlong builds: a value that fits int64 becomes an int; anything
    // bigger is wrapped in Tag(2/3, bytes) (D4: no exception for
    // well-formed CBOR). The magnitude fits 64 bits iff n < 8, or n == 8
    // with the top bit clear (negative may use the full 64 bits).
    bool fits;
    uint64_t mag = 0;
    for (size_t i = 0; i < n && i < 8; i++) {
        mag = (mag << 8) | b[i];
    }
    if (n < 8) {
        fits = true;
    } else if (n == 8) {
        fits = negative || (b[0] & 0x80) == 0;
    } else {
        fits = false;
    }
    if (fits) {
        if (negative) {
            // -1-mag; mag <= 2**64-1, so -1-mag >= -2**64 fits int64.
            if (u->stream != MP_OBJ_NULL) {
                vstr_clear(&vstr);
            }
            return mp_obj_new_int_from_ll(-1 - (long long)mag);
        }
        if (mag <= 0x7fffffffffffffffULL) {
            if (u->stream != MP_OBJ_NULL) {
                vstr_clear(&vstr);
            }
            return mp_obj_new_int_from_ll((long long)mag);
        }
        // 2**63..2**64-1: no int64 representation; wrap in Tag(2, bytes).
    }
    // The Tag content is a copy of the magnitude bytes (b may point into
    // the input buffer or into vstr; mp_obj_new_bytes copies either way,
    // and only then may the stream vstr be released).
    mp_obj_t content = n == 0 ? mp_const_empty_bytes : mp_obj_new_bytes(b, n);
    if (u->stream != MP_OBJ_NULL) {
        vstr_clear(&vstr);
    }
    mp_obj_tag_t *t = mp_obj_malloc(mp_obj_tag_t, &ccbor_tag_type);
    t->tag = MP_OBJ_NEW_SMALL_INT(negative ? 3 : 2);
    t->value = content;
    return MP_OBJ_FROM_PTR(t);
    #endif
}

// Tag content dispatch (D2): registry first (tags 2/3 are intercepted
// before the registry by the caller), then the Tag wrapper. The 55799
// self-describe tag is stripped like cbor2 does (also nested: stripping
// happens here, on the way up).
static mp_obj_t up_tag_dispatch(unpacker_t *u, mp_obj_t tag_num, mp_obj_t content) {
    if (mp_obj_equal(tag_num, MP_OBJ_NEW_SMALL_INT(55799))) {
        return content; // self-describe: strip
    }
    mp_map_elem_t *e = NULL;
    if (u->tags != NULL) {
        e = mp_map_lookup(u->tags, tag_num, MP_MAP_LOOKUP);
    }
    if (e != NULL) {
        mp_obj_t cls = e->value;
        mp_obj_t dest[4];
        mp_load_method_maybe(cls, MP_QSTR_unpackb, dest);
        if (dest[0] == MP_OBJ_NULL) {
            raise_with_obj(&mp_type_NotImplementedError, "Tag class ", cls, PRINT_REPR, " lacks unpackb()");
        }
        dest[2] = content;
        dest[3] = options_dict(&u->options, u->kw);
        mp_obj_t obj = mp_call_method_n_kw(2, 0, dest);
        up_revalidate(u);
        return obj;
    }
    mp_obj_tag_t *t = mp_obj_malloc(mp_obj_tag_t, &ccbor_tag_type);
    t->tag = tag_num;
    t->value = content;
    return MP_OBJ_FROM_PTR(t);
}

static void up_enter(unpacker_t *u) {
    MP_STACK_CHECK();
    if (++u->depth > CCBOR_MAX_DEPTH) {
        raise_depth();
    }
}

// How many items can be preallocated for a container announcing n of them.
// Each item takes at least one byte, so in buffer mode a larger n cannot be
// genuine: allocate only what the data could hold and fail when it runs out.
static size_t up_prealloc(unpacker_t *u, size_t n) {
    size_t limit = u->stream == MP_OBJ_NULL ? (size_t)(u->end - u->cur) : 64;
    return n < limit ? n : limit;
}

// SIZE_MAX as n means an indefinite-length array: items until the BREAK
// byte (0xff). up_obj reports it with a sentinel that no real object can
// ever be: a pointer to a static dummy (a small int is an immediate value,
// so even int 0 must not collide with it).
static const mp_obj_base_t ccbor_break_sentinel_obj = {&mp_type_object};
#define CCBOR_BREAK_SENTINEL MP_OBJ_FROM_PTR(&ccbor_break_sentinel_obj)

static mp_obj_t up_array(unpacker_t *u, size_t n) {
    bool indef = n == SIZE_MAX;
    if (indef) {
        n = 0; // grown by append; prealloc makes no sense
    }
    up_enter(u);
    mp_obj_t res;
    if (indef) {
        res = mp_obj_new_list(0, NULL);
        u->indef++;
        while (true) {
            mp_obj_t item = up_obj(u);
            if (item == CCBOR_BREAK_SENTINEL) {
                break;
            }
            mp_obj_list_append(res, item);
        }
        u->indef--;
        if (u->use_tuple) {
            mp_obj_list_t *l = MP_OBJ_TO_PTR(res);
            res = mp_obj_new_tuple(l->len, l->items);
        }
        u->depth--;
        return res;
    }
    if (up_prealloc(u, n) == n) {
        // The usual case: items go straight into a list or tuple of size n.
        mp_obj_t *items;
        if (u->use_tuple) {
            res = mp_obj_new_tuple(n, NULL);
            items = ((mp_obj_tuple_t *)MP_OBJ_TO_PTR(res))->items;
        } else {
            res = mp_obj_new_list(n, NULL);
            items = ((mp_obj_list_t *)MP_OBJ_TO_PTR(res))->items;
        }
        for (size_t i = 0; i < n; i++) {
            mp_obj_t item = up_obj(u);
            if (item == CCBOR_BREAK_SENTINEL) {
                raise_invalid_code(0xff); // BREAK inside a definite container
            }
            items[i] = item;
        }
    } else {
        res = mp_obj_new_list(0, NULL);
        for (size_t i = 0; i < n; i++) {
            mp_obj_t item = up_obj(u);
            if (item == CCBOR_BREAK_SENTINEL) {
                raise_invalid_code(0xff);
            }
            mp_obj_list_append(res, item);
        }
        if (u->use_tuple) {
            mp_obj_list_t *l = MP_OBJ_TO_PTR(res);
            res = mp_obj_new_tuple(l->len, l->items);
        }
    }
    u->depth--;
    return res;
}

// A list used as a map key becomes a tuple, recursively (umsgpack's
// _deep_list_to_tuple).
static mp_obj_t list_to_tuple(mp_obj_t o) {
    if (!mp_obj_is_type(o, &mp_type_list)) {
        return o;
    }
    MP_STACK_CHECK();
    mp_obj_list_t *l = MP_OBJ_TO_PTR(o);
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(l->len, NULL));
    for (size_t i = 0; i < l->len; i++) {
        t->items[i] = list_to_tuple(l->items[i]);
    }
    return MP_OBJ_FROM_PTR(t);
}

static bool key_is_simple(mp_obj_t k) {
    return mp_obj_is_small_int(k) || mp_obj_is_qstr(k) || mp_obj_is_float(k)
           || k == mp_const_none || k == mp_const_true || k == mp_const_false
           || mp_obj_is_exact_type(k, &mp_type_str) || mp_obj_is_exact_type(k, &mp_type_bytes)
           || mp_obj_is_exact_type(k, &mp_type_int);
}

static void up_check_hashable(unpacker_t *u, mp_obj_t k) {
    if (key_is_simple(k)) {
        return;
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_unary_op(MP_UNARY_OP_HASH, k);
        nlr_pop();
    } else {
        raise_with_obj(&mp_type_ccbor_UnhashableKeyException, "\"", k, PRINT_STR, "\"");
    }
    up_revalidate(u);
}

static mp_obj_t up_map(unpacker_t *u, size_t n) {
    bool indef = n == SIZE_MAX;
    up_enter(u);
    mp_obj_t d = mp_obj_new_dict(indef ? 0 : up_prealloc(u, n));
    #if MICROPY_PY_COLLECTIONS_ORDEREDDICT
    if (u->use_ordered_dict) {
        mp_obj_dict_t *od = MP_OBJ_TO_PTR(d);
        od->base.type = &mp_type_ordereddict;
        od->map.is_ordered = 1;
    }
    #else
    if (u->use_ordered_dict) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("OrderedDict not available"));
    }
    #endif
    mp_map_t *map = mp_obj_dict_get_map(d);
    if (indef) {
        u->indef++;
    }
    for (size_t i = 0; indef || i < n; i++) {
        u->key = true;
        mp_obj_t k = up_obj(u);
        u->key = false; // the value (and anything after) is no key material
        if (k == CCBOR_BREAK_SENTINEL) {
            if (indef) {
                break; // the BREAK byte, not an object
            }
            raise_invalid_code(0xff); // BREAK inside a definite container
        }
        k = list_to_tuple(k);
        up_check_hashable(u, k);
        // A single lookup both detects a duplicate and reserves the slot. The
        // slot stays valid while the value is decoded: nothing else can reach
        // this dict until it is returned.
        mp_map_elem_t *e = mp_map_lookup(map, k, MP_MAP_LOOKUP_ADD_IF_NOT_FOUND);
        if (!key_is_simple(k)) {
            up_revalidate(u);
        }
        if (e->value != MP_OBJ_NULL) {
            vstr_t vstr;
            mp_print_t print;
            vstr_init_print(&vstr, 32, &print);
            mp_print_str(&print, "\"");
            mp_obj_print_helper(&print, k, PRINT_STR);
            mp_print_str(&print, "\" (");
            mp_obj_print_helper(&print, MP_OBJ_FROM_PTR(mp_obj_get_type(k)), PRINT_STR);
            mp_print_str(&print, ")");
            nlr_raise(mp_obj_new_exception_arg1(&mp_type_ccbor_DuplicateKeyException,
                mp_obj_new_str_from_vstr(&vstr)));
        }
        mp_obj_t v = up_obj(u);
        if (v == CCBOR_BREAK_SENTINEL) {
            // BREAK where the value belongs: the map ended on an odd item
            // count, which is not well-formed (RFC 8949 2.2.1).
            raise_invalid_code(0xff);
        }
        e->value = v;
    }
    if (indef) {
        u->indef--;
    }
    u->depth--;
    return d;
}

static mp_obj_t up_float32(unpacker_t *u) {
    #if MICROPY_PY_BUILTINS_FLOAT
    uint32_t x = up_be(u, 4);
    float f;
    memcpy(&f, &x, sizeof(f));
    return mp_obj_new_float_from_f(f);
    #else
    (void)u;
    raise_unsupported_msg(MP_ERROR_TEXT("float not supported"));
    #endif
}

static mp_obj_t up_float64(unpacker_t *u) {
    #if MICROPY_PY_BUILTINS_FLOAT
    uint64_t x = up_be64(u);
    double d;
    memcpy(&d, &x, sizeof(d));
    return mp_obj_new_float_from_d(d);
    #else
    (void)u;
    raise_unsupported_msg(MP_ERROR_TEXT("float not supported"));
    #endif
}

// Indefinite-length text (0x7f..ff) or byte string (0x5f..ff): chunks of
// the same major type, each definite-length, joined. A chunk of another
// type or another indefinite item inside is an invalid code (D7).
static mp_obj_t up_str_indef(unpacker_t *u, bool text, bool key) {
    up_enter(u);
    vstr_t vstr;
    vstr_init(&vstr, 32);
    while (true) {
        byte ic = up_read(u, 1)[0];
        if (ic == 0xff) {
            break;
        }
        byte mt = ic & 0xe0;
        byte ai = ic & 0x1f;
        if (mt != (text ? 0x60 : 0x40) || ai == 0x1f) {
            raise_invalid_code(ic);
        }
        size_t n;
        if (ai < 24) {
            n = ai;
        } else if (ai <= 27) {
            n = (size_t)up_be(u, 1 << (ai - 24));
        } else {
            raise_invalid_code(ic);
        }
        // Append the chunk.
        size_t old = vstr.len;
        vstr_add_len(&vstr, n);
        if (u->stream == MP_OBJ_NULL) {
            if ((size_t)(u->end - u->cur) < n) {
                raise_insufficient();
            }
            memcpy(vstr.buf + old, u->cur, n);
            u->cur += n;
        } else {
            up_stream_read(u, (byte *)vstr.buf + old, n);
        }
    }
    u->depth--;
    if (text) {
        #if MICROPY_PY_BUILTINS_STR_UNICODE && MICROPY_PY_BUILTINS_STR_UNICODE_CHECK
        if (!unicode_encoding_check(MP_ENCODING_UTF8, (const byte *)vstr.buf, vstr.len)) {
            if (u->allow_invalid_utf8) {
                return mp_obj_new_bytes_from_vstr(&vstr);
            }
            mp_raise_msg(&mp_type_ccbor_InvalidStringException, MP_ERROR_TEXT("unpacked string is invalid utf-8"));
        }
        #endif
        #if CCBOR_REUSE_QSTR
        qstr q = key ? qstr_find_strn(vstr.buf, vstr.len) : MP_QSTRnull;
        if (q != MP_QSTRnull) {
            vstr_clear(&vstr);
            return MP_OBJ_NEW_QSTR(q);
        }
        #else
        (void)key;
        #endif
        return mp_obj_new_str_from_vstr(&vstr);
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}

static mp_obj_t up_obj(unpacker_t *u) {
    // u->key stays as it is: a map key's whole subtree must decode
    // hashable (byte strings copy, not views), and up_map clears the
    // flag before the value. The local copies it for this object only.
    bool key = u->key;
    byte ic = up_read(u, 1)[0];
    byte mt = ic & 0xe0;
    byte ai = ic & 0x1f;
    if (ic == 0xff) {
        // BREAK: valid only as the end of an indefinite container (the
        // caller checks u->indef; a stray one raises here).
        if (u->indef == 0) {
            raise_invalid_code(ic);
        }
        return CCBOR_BREAK_SENTINEL;
    }
    // The argument of a definite-length item: 0..2**64-1.
    #define UP_ARG() \
    (ai < 24 ? (uint64_t)ai \
            : ai == 24 ? (uint64_t)up_be(u, 1) \
            : ai == 25 ? (uint64_t)up_be(u, 2) \
            : ai == 26 ? (uint64_t)up_be(u, 4) \
            : ai == 27 ? up_be64(u) : 0)
    if (ai >= 28 && ai <= 30) {
        // Additional information 28-30: not well-formed in any major type.
        raise_invalid_code(ic);
    }
    if (ai == 31 && mt < 0x40) {
        // Indefinite length exists only for strings and containers; the
        // BREAK byte itself is handled above. mt0/mt1 (and tags, which
        // check ai==31 explicitly) cannot be indefinite.
        raise_invalid_code(ic);
    }
    switch (mt) {
        case 0x00: { // unsigned int
            uint64_t x = UP_ARG();
            mp_obj_t v = up_uint64_obj(x);
            if (v != MP_OBJ_NULL) {
                return v;
            }
            // longlong build, x >= 2**63: wrap in Tag(2, magnitude bytes)
            mp_obj_tag_t *t = mp_obj_malloc(mp_obj_tag_t, &ccbor_tag_type);
            t->tag = MP_OBJ_NEW_SMALL_INT(2);
            byte *bs = (byte *)m_new(byte, 9);
            bs[0] = 0;
            put_be64(bs + 1, x);
            // trim to the minimal byte string
            size_t nb = 9;
            while (nb > 1 && bs[9 - nb] == 0) {
                nb--;
            }
            t->value = mp_obj_new_bytes(bs + 9 - nb, nb);
            m_del(byte, bs, 9);
            return MP_OBJ_FROM_PTR(t);
        }
        case 0x20: { // negative int
            uint64_t n = UP_ARG();
            mp_obj_t v = up_negint_obj(n);
            if (v != MP_OBJ_NULL) {
                return v;
            }
            // longlong build, below LONG_LONG_MIN: wrap in Tag(3, bytes of n)
            mp_obj_tag_t *t = mp_obj_malloc(mp_obj_tag_t, &ccbor_tag_type);
            t->tag = MP_OBJ_NEW_SMALL_INT(3);
            byte *bs = (byte *)m_new(byte, 8);
            put_be64(bs, n);
            t->value = mp_obj_new_bytes(bs, 8);
            m_del(byte, bs, 8);
            return MP_OBJ_FROM_PTR(t);
        }
        case 0x40: // byte string
            if (ai == 31) {
                return up_str_indef(u, false, false);
            }
            return up_bin(u, (size_t)UP_ARG(), key);
        case 0x60: // text string
            if (ai == 31) {
                return up_str_indef(u, true, key);
            }
            return up_str(u, (size_t)UP_ARG(), key);
        case 0x80: // array
            if (ai == 31) {
                return up_array(u, SIZE_MAX);
            }
            return up_array(u, (size_t)UP_ARG());
        case 0xa0: // map
            if (ai == 31) {
                return up_map(u, SIZE_MAX);
            }
            return up_map(u, (size_t)UP_ARG());
        case 0xc0: { // tag
            uint64_t n = UP_ARG();
            if (ai == 31) {
                raise_invalid_code(ic); // indefinite tag numbers do not exist
            }
            mp_obj_t tag_num = up_uint64_obj(n);
            if (tag_num == MP_OBJ_NULL) {
                // longlong build, tag number >= 2**63: keep the raw uint64
                // in the Tag (review point 10). A small-int tag field cannot
                // hold it; use the 8-byte byte string form? No: the number
                // is a tag NUMBER; store it as an mpz-free 64-bit value in
                // the Tag object. MicroPython longlong builds have no int
                // for it, so the Tag keeps the raw bytes.
                // A longlong build has no int for n >= 2**63: the tag
                // number keeps its raw 64-bit value in a small wrapper...
                // there is none, so this raises (documented limitation).
                raise_unsupported_msg(MP_ERROR_TEXT("tag number too large for this build"));
            }
            // Native bignum tags 2/3: the content must be a byte string.
            if (n == 2 || n == 3) {
                byte cic = up_read(u, 1)[0];
                byte cmt = cic & 0xe0;
                byte cai = cic & 0x1f;
                if (cmt != 0x40 || cai == 0x1f || (cai >= 28 && cai <= 30)) {
                    raise_invalid_code(cic);
                }
                size_t cn = cai < 24 ? cai : (size_t)(cai == 24 ? up_be(u, 1) : cai == 25 ? up_be(u, 2) : cai == 26 ? up_be(u, 4) : up_be64(u));
                return up_bignum(u, cn, n == 3);
            }
            up_enter(u); // tags count towards the depth limit (review 8)
            mp_obj_t content = up_obj(u);
            u->depth--;
            return up_tag_dispatch(u, tag_num, content);
        }
        default: { // 0xe0: simple values and floats
            // ai < 24: a simple value encoded in this byte. 20..23 are the
            // known ones; 0..19 are unassigned and unsupported (D3).
            if (ai < 24) {
                switch (ai) {
                    case 20:
                        return mp_const_false;
                    case 21:
                        return mp_const_true;
                    case 22:
                        return mp_const_none;
                    case 23:
                        return MP_OBJ_FROM_PTR(&mp_const_ellipsis_obj);
                    default:
                        raise_unsupported_simple(ai);
                }
            }
            switch (ai) {
                case 20:
                    return mp_const_false;
                case 21:
                    return mp_const_true;
                case 22:
                    return mp_const_none;
                case 23:
                    return MP_OBJ_FROM_PTR(&mp_const_ellipsis_obj);
                case 24: { // 1-byte simple value
                    byte sv = up_read(u, 1)[0];
                    // 0..31 duplicate the 0-byte forms and are reserved
                    // (not well-formed); 32..255 are unassigned (D3, D7).
                    if (sv < 32) {
                        raise_invalid_code(ic);
                    }
                    raise_unsupported_simple(sv);
                }
                case 25: { // half float
                    #if MICROPY_PY_BUILTINS_FLOAT
                    uint32_t h = up_be(u, 2);
                    return mp_obj_new_float_from_f(f16_bits_to_f32(h));
                    #else
                    (void)up_be(u, 2);
                    raise_unsupported_msg(MP_ERROR_TEXT("float not supported"));
                    #endif
                }
                case 26:
                    return up_float32(u);
                case 27:
                    return up_float64(u);
                case 28:
                case 29:
                case 30:
                    raise_invalid_code(ic); // (already caught above; kept for clarity)
                case 31:
                    raise_invalid_code(ic); // stray BREAK
                default:
                    raise_invalid_code(ic);
            }
        }
    }
    raise_invalid_code(ic); // unreachable
}

static void unpacker_init(unpacker_t *u, mp_map_t *kw) {
    u->src = MP_OBJ_NULL;
    u->view_src = MP_OBJ_NULL;
    u->stream = MP_OBJ_NULL;
    u->sp = NULL;
    u->kw = kw;
    u->options = MP_OBJ_NULL;
    u->depth = 0;
    u->indef = 0;
    u->key = false;
    u->use_tuple = kw_flag(kw, MP_QSTR_use_tuple);
    u->use_ordered_dict = kw_flag(kw, MP_QSTR_use_ordered_dict);
    u->allow_invalid_utf8 = kw_flag(kw, MP_QSTR_allow_invalid_utf8);
    u->memory_min = 0;
    mp_obj_t mm = kw_get(kw, MP_QSTR_memory_min);
    if (mm != MP_OBJ_NULL) {
        mp_int_t n = mp_obj_get_int(mm);
        if (n < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("memory_min must not be negative"));
        }
        #if MICROPY_PY_BUILTINS_MEMORYVIEW
        u->memory_min = n;
        #else
        // No memoryview on this build: decode copies, whatever the option
        // says. Code that passes memory_min must run on both kinds.
        #endif
    }
    u->tags = ext_map(MP_QSTR_tags);
}

static void unpacker_set_buffer(unpacker_t *u, mp_obj_t src, const byte *buf, size_t len) {
    u->base = buf;
    u->cur = buf;
    u->end = buf + len;
    // bytes are immutable: no need to re-validate the buffer after callbacks.
    u->src = mp_obj_is_type(src, &mp_type_bytes) ? MP_OBJ_NULL : src;
    // The buffer belongs to the caller, so memory_min views can refer to it.
    u->view_src = src;
}

// loads(data, **options): data is bytes, bytearray or any other read buffer
// except str. Trailing bytes after the first object are ignored (as umsgpack).
static mp_obj_t ccbor_loads(size_t n_args, const mp_obj_t *args, mp_map_t *kw) {
    mp_arg_check_num(n_args, kw->used, 1, 1, true);
    mp_buffer_info_t bi;
    if (mp_obj_is_str(args[0]) || !mp_get_buffer(args[0], &bi, MP_BUFFER_READ)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Need 'bytes' or 'bytearray'"));
    }
    unpacker_t u;
    unpacker_init(&u, kw);
    unpacker_set_buffer(&u, args[0], bi.buf, bi.len);
    return up_obj(&u);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ccbor_loads_obj, 1, ccbor_loads);

// load(fp, **options): reads exactly the bytes of one object from fp.
static mp_obj_t ccbor_load(size_t n_args, const mp_obj_t *args, mp_map_t *kw) {
    mp_arg_check_num(n_args, kw->used, 1, 1, true);
    // A stream has no buffer that outlives the call, so nothing to view.
    if (kw_get(kw, MP_QSTR_memory_min) != MP_OBJ_NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("memory_min is only supported by loads()"));
    }
    unpacker_t u;
    unpacker_init(&u, kw);
    u.stream = args[0];
    u.sp = native_stream(u.stream, MP_QSTR_read, &mp_stream_read_obj, u.read_meth);
    if (u.sp != NULL && u.sp->read == NULL) {
        u.sp = NULL;
    }
    return up_obj(&u);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ccbor_load_obj, 1, ccbor_load);
/******************************************************************************/
// Size of the first complete object in buf, or 0 if more data is needed.
// Iterative and allocation free: it only walks the headers, counting the
// items still expected, so deep nesting costs no C stack (except for the
// small fixed stack of open INDEFINITE containers, review point 9).
// Returns SIZE_MAX for a not-well-formed code (the caller skips a byte
// and raises InvalidCodeException, so the stream can resynchronise).
//
// Accounting: pending counts the items the innermost DEFINITE context still
// owes (starting at 1 for the top-level object); opening an indefinite
// container (5f/7f/9f/bf) pushes the parent's pending and resets it to 0;
// BREAK (ff) requires pending == 0 and an open frame, restores the parent's
// pending, and the container counts as the one item its opening byte
// consumed. Definite containers add their item count to pending.

typedef struct _measure_frame_t {
    uint64_t pending; // the parent context's outstanding item count
} measure_frame_t;

static size_t ccbor_measure(const byte *buf, size_t len) {
    const byte *p = buf;
    const byte *end = buf + len;
    uint64_t pending = 1;
    measure_frame_t stack[CCBOR_MAX_DEPTH];
    size_t sp = 0;
    while (pending > 0 || sp > 0) {
        if (p == end) {
            return 0;
        }
        byte ic = *p++;
        if (pending > 0) {
            pending--; // this byte starts one expected item
        }
        if (ic == 0xff) {
            // BREAK: closes the innermost indefinite container. Valid only
            // with an open frame and no items still owed inside it.
            if (sp == 0 || pending != 0) {
                return SIZE_MAX;
            }
            sp--;
            pending = stack[sp].pending;
            continue;
        }
        byte mt = ic & 0xe0;
        byte ai = ic & 0x1f;
        uint64_t payload = 0; // bytes after the header
        uint64_t items = 0;   // children still to read
        if (ai >= 28 && ai <= 30) {
            return SIZE_MAX; // additional info 28-30: invalid everywhere
        }
        if (ai == 31) {
            // Indefinite length: only strings and containers.
            if (mt < 0x40 || mt == 0xc0) {
                return SIZE_MAX;
            }
            if (mt == 0x40 || mt == 0x60) {
                // Indefinite string: scan the chunks (definite-length
                // strings of the same major type) until BREAK. No frame
                // is needed: chunks cannot nest.
                while (true) {
                    if (p == end) {
                        return 0;
                    }
                    byte cic = *p++;
                    if (cic == 0xff) {
                        break;
                    }
                    byte cmt = cic & 0xe0;
                    byte cai = cic & 0x1f;
                    if (cmt != mt || cai >= 28) {
                        return SIZE_MAX; // wrong chunk type or bad code
                    }
                    uint64_t cn = cai < 24 ? cai : 0; // the chunk length
                    size_t chdr = cai < 24 ? 0 : (size_t)1 << (cai - 24);
                    if (cai == 31 || (size_t)(end - p) < chdr) {
                        return cai == 31 ? SIZE_MAX : 0;
                    }
                    for (size_t i = 0; i < chdr; i++) {
                        cn = (cn << 8) | *p++;
                    }
                    if (cn > (uint64_t)(end - p)) {
                        return 0; // chunk longer than the data
                    }
                    p += cn;
                }
                continue; // the string was one item of this context
            }
            // Indefinite array or map: push the parent context.
            if (sp >= CCBOR_MAX_DEPTH) {
                return SIZE_MAX; // deeper than the limit
            }
            stack[sp].pending = pending;
            sp++;
            pending = 0; // items until BREAK
            continue;
        }
        // mt7 floats have no argument: the bytes after the head are the
        // payload (f9: 2, fa: 4, fb: 8).
        if (mt == 0xe0 && ai >= 25) {
            size_t flen = ai == 25 ? 2 : ai == 26 ? 4 : 8;
            if ((size_t)(end - p) < flen) {
                return 0;
            }
            p += flen;
            continue;
        }
        // Definite length: ai < 24 encodes the argument in this byte;
        // 24/25/26/27 add 1/2/4/8 bytes of it.
        uint64_t n = ai;
        if (ai >= 24) {
            size_t hdr = (size_t)1 << (ai - 24);
            if ((size_t)(end - p) < hdr) {
                return 0;
            }
            n = 0;
            for (size_t i = 0; i < hdr; i++) {
                n = (n << 8) | *p++;
            }
        }
        switch (mt) {
            case 0x00:
            case 0x20:
                continue; // int: no payload
            case 0x40:
            case 0x60:
                payload = n;
                break;
            case 0x80:
                items = n;
                break;
            case 0xa0:
                items = 2 * n; // n <= 2**63: no overflow
                break;
            case 0xc0:
                items = 1; // the tag's content
                break;
            default: { // 0xe0: simple values and floats
                if (ai < 24) {
                    // Simple values 0..23 in one byte. 20..23 are the
                    // known ones; 0..19 are unassigned (the decoder raises
                    // UnsupportedTypeException): either way, one byte.
                    continue;
                }
                if (ai == 24) {
                    if (n < 32) {
                        return SIZE_MAX; // reserved 1-byte simple value
                    }
                    continue; // the decoder raises for the unassigned ones
                }
                // 25/26/27: half/single/double float
                payload = ai == 25 ? 2 : ai == 26 ? 4 : 8;
                break;
            }
        }
        if (payload > 0) {
            if ((uint64_t)(end - p) < payload) {
                return 0;
            }
            p += payload;
        }
        pending += items;
    }
    return p - buf;
}

typedef struct _mp_obj_unpacker_t {
    mp_obj_base_t base;
    vstr_t buf;            // fed data; [pos, buf.len) not consumed yet
    size_t pos;
    size_t max_buffer_size; // 0: unlimited
    size_t checked_len;     // pending bytes at the last incomplete measure
    mp_obj_t kw_dict;       // the load options (a dict)
    bool busy;              // decoding: feed() would move the buffer
} mp_obj_unpacker_t;

static mp_obj_t unpacker_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, true);
    mp_obj_unpacker_t *self = mp_obj_malloc(mp_obj_unpacker_t, type);
    vstr_init(&self->buf, 64);
    self->pos = 0;
    self->max_buffer_size = 0;
    self->checked_len = SIZE_MAX;
    self->busy = false;
    // Keep the options as a dict: the map in args is only valid for this call.
    self->kw_dict = mp_obj_new_dict(n_kw);
    for (size_t i = 0; i < n_kw; i++) {
        mp_obj_t key = args[n_args + 2 * i];
        mp_obj_t value = args[n_args + 2 * i + 1];
        if (mp_obj_equal(key, MP_OBJ_NEW_QSTR(MP_QSTR_max_buffer_size))) {
            self->max_buffer_size = mp_obj_get_int(value);
        } else {
            if (mp_obj_equal(key, MP_OBJ_NEW_QSTR(MP_QSTR_memory_min))) {
                // feed() compacts and grows the buffer, which would silently
                // invalidate every view. Use next_raw() and loads() instead.
                mp_raise_ValueError(MP_ERROR_TEXT("memory_min is only supported by loads()"));
            }
            mp_obj_dict_store(self->kw_dict, key, value);
        }
    }
    return MP_OBJ_FROM_PTR(self);
}

static void unpacker_check_idle(mp_obj_unpacker_t *self) {
    if (self->busy) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Unpacker is busy"));
    }
}

// feed(data): append bytes to the internal buffer.
static mp_obj_t unpacker_feed(mp_obj_t self_in, mp_obj_t data) {
    mp_obj_unpacker_t *self = MP_OBJ_TO_PTR(self_in);
    unpacker_check_idle(self);
    mp_buffer_info_t bi;
    mp_get_buffer_raise(data, &bi, MP_BUFFER_READ);
    size_t pending = self->buf.len - self->pos;
    if (self->max_buffer_size != 0 && bi.len > self->max_buffer_size - pending) {
        mp_raise_ValueError(MP_ERROR_TEXT("Unpacker buffer full"));
    }
    if (self->pos > 0) {
        memmove(self->buf.buf, self->buf.buf + self->pos, pending);
        self->buf.len = pending;
        self->pos = 0;
    }
    vstr_add_strn(&self->buf, bi.buf, bi.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(unpacker_feed_obj, unpacker_feed);

// Size of the next complete object, 0 if there is none yet.
static size_t unpacker_next_size(mp_obj_unpacker_t *self) {
    size_t pending = self->buf.len - self->pos;
    if (pending == self->checked_len) {
        return 0; // nothing new since the last incomplete check
    }
    size_t n = ccbor_measure((const byte *)self->buf.buf + self->pos, pending);
    if (n == SIZE_MAX) {
        // Skip the bad byte so the stream can resynchronise.
        self->pos += 1;
        self->checked_len = SIZE_MAX;
        mp_raise_msg(&mp_type_ccbor_InvalidCodeException, MP_ERROR_TEXT("invalid code"));
    }
    self->checked_len = n == 0 ? pending : SIZE_MAX;
    return n;
}

static mp_obj_t unpacker_iternext(mp_obj_t self_in) {
    mp_obj_unpacker_t *self = MP_OBJ_TO_PTR(self_in);
    unpacker_check_idle(self);
    size_t n = unpacker_next_size(self);
    if (n == 0) {
        return MP_OBJ_STOP_ITERATION;
    }
    const byte *start = (const byte *)self->buf.buf + self->pos;
    // Consume the object first: if decoding raises, the stream moves on.
    self->pos += n;
    unpacker_t u;
    unpacker_init(&u, mp_obj_dict_get_map(self->kw_dict));
    // No re-validation needed: feed() cannot move the buffer while busy.
    u.base = u.cur = start;
    u.end = start + n;
    self->busy = true;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t obj = up_obj(&u);
        nlr_pop();
        self->busy = false;
        return obj;
    } else {
        self->busy = false;
        nlr_jump(nlr.ret_val);
    }
}

// next_raw(): the bytes of the next complete object, without decoding it, or
// None if no complete object has been fed yet.
static mp_obj_t unpacker_next_raw(mp_obj_t self_in) {
    mp_obj_unpacker_t *self = MP_OBJ_TO_PTR(self_in);
    unpacker_check_idle(self);
    size_t n = unpacker_next_size(self);
    if (n == 0) {
        return mp_const_none;
    }
    mp_obj_t raw = mp_obj_new_bytes((const byte *)self->buf.buf + self->pos, n);
    self->pos += n;
    return raw;
}
static MP_DEFINE_CONST_FUN_OBJ_1(unpacker_next_raw_obj, unpacker_next_raw);

// reset(): discard all buffered data.
static mp_obj_t unpacker_reset(mp_obj_t self_in) {
    mp_obj_unpacker_t *self = MP_OBJ_TO_PTR(self_in);
    unpacker_check_idle(self);
    self->buf.len = 0;
    self->pos = 0;
    self->checked_len = SIZE_MAX;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(unpacker_reset_obj, unpacker_reset);

// len(unpacker): bytes buffered and not consumed yet.
static mp_obj_t unpacker_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    mp_obj_unpacker_t *self = MP_OBJ_TO_PTR(self_in);
    switch (op) {
        case MP_UNARY_OP_LEN:
            return MP_OBJ_NEW_SMALL_INT(self->buf.len - self->pos);
        case MP_UNARY_OP_BOOL:
            return mp_obj_new_bool(self->buf.len != self->pos);
        case MP_UNARY_OP_HASH:
            // As for any object without __hash__: its identity.
            return MP_OBJ_NEW_SMALL_INT((mp_uint_t)self_in);
        default:
            return MP_OBJ_NULL;
    }
}

static const mp_rom_map_elem_t unpacker_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_feed), MP_ROM_PTR(&unpacker_feed_obj) },
    { MP_ROM_QSTR(MP_QSTR_next_raw), MP_ROM_PTR(&unpacker_next_raw_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&unpacker_reset_obj) },
};
static MP_DEFINE_CONST_DICT(unpacker_locals_dict, unpacker_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ccbor_unpacker_type,
    MP_QSTR_Unpacker,
    MP_TYPE_FLAG_ITER_IS_ITERNEXT,
    make_new, unpacker_make_new,
    iter, unpacker_iternext,
    unary_op, unpacker_unary_op,
    locals_dict, &unpacker_locals_dict
    );

/******************************************************************************/
// tag_serializable(tag_number, example=None): class decorator.
//
// Registers a class for a CBOR tag number (0..2**64-1; tags 2 and 3 are
// the native bignum tags and are rejected). With example= a built-in type
// is registered instead: its instances (and instances of its subclasses)
// pack through the class's packb(obj, options), like umsgpack's ext mechanism.

static mp_obj_t tag_register(size_t n_args, const mp_obj_t *args) {
    // args: tag_number, example (closed over by the decorator), cls
    mp_obj_t tag_num = args[0];
    mp_obj_t example = args[1];
    mp_obj_t cls = args[2];
    if (!mp_obj_is_int(tag_num)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Tag number is not type integer"));
    }
    // The number must be 0..2**64-1; on longlong builds an int cannot
    // exceed 2**63-1, so >= 0 suffices there. Same rule as Tag().
    if (mp_obj_int_sign(tag_num) < 0) {
        raise_with_obj(&mp_type_ValueError, "Tag number ", tag_num, PRINT_STR,
            " is negative");
    }
    #if MICROPY_LONGINT_IMPL == MICROPY_LONGINT_IMPL_MPZ
    if (!mp_obj_is_small_int(tag_num)
        && ((mp_obj_int_t *)MP_OBJ_TO_PTR(tag_num))->mpz.len * MPZ_DIG_SIZE > 64) {
        raise_with_obj(&mp_type_ValueError, "Tag number ", tag_num, PRINT_STR,
            " is out of range of 0 to 2**64-1");
    }
    #endif
    // Tags 2 and 3 are the native bignum tags: registering them is an error
    // (the decoder always converts them to int).
    if (mp_obj_is_small_int(tag_num)
        && (MP_OBJ_SMALL_INT_VALUE(tag_num) == 2 || MP_OBJ_SMALL_INT_VALUE(tag_num) == 3)) {
        mp_raise_ValueError(MP_ERROR_TEXT("tag 2 and 3 are the native bignum tags"));
    }
    mp_obj_t reg = ext_registry(true);
    mp_obj_t tags = ext_dict(reg, MP_QSTR_tags);
    mp_obj_t custom = ext_dict(reg, MP_QSTR_custom);
    mp_map_elem_t *e = mp_map_lookup(mp_obj_dict_get_map(tags), tag_num, MP_MAP_LOOKUP);
    if (e != NULL) {
        vstr_t vstr;
        mp_print_t print;
        vstr_init_print(&vstr, 48, &print);
        mp_print_str(&print, "Tag number ");
        mp_obj_print_helper(&print, tag_num, PRINT_STR);
        mp_print_str(&print, " already registered with class ");
        mp_obj_print_helper(&print, e->value, PRINT_REPR);
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_ValueError, mp_obj_new_str_from_vstr(&vstr)));
    }
    if (mp_map_lookup(mp_obj_dict_get_map(custom), cls, MP_MAP_LOOKUP) != NULL) {
        vstr_t vstr;
        mp_print_t print;
        vstr_init_print(&vstr, 48, &print);
        mp_print_str(&print, "Class ");
        mp_obj_print_helper(&print, cls, PRINT_REPR);
        mp_print_str(&print, " already registered with tag number ");
        mp_obj_print_helper(&print, tag_num, PRINT_STR);
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_ValueError, mp_obj_new_str_from_vstr(&vstr)));
    }
    mp_obj_dict_store(tags, tag_num, cls);
    if (example == mp_const_none) {
        mp_obj_dict_store(custom, cls, tag_num);
    } else {
        mp_obj_t pair[2] = { cls, tag_num };
        mp_obj_dict_store(ext_dict(reg, MP_QSTR_builtins), example, mp_obj_new_tuple(2, pair));
    }
    return cls;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tag_register_obj, 3, 3, tag_register);

static mp_obj_t ccbor_tag_serializable(size_t n_args, const mp_obj_t *args) {
    mp_obj_t closed[2] = { args[0], n_args > 1 ? args[1] : mp_const_none };
    return mp_obj_new_closure(MP_OBJ_FROM_PTR(&tag_register_obj), 2, closed);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ccbor_tag_serializable_obj, 1, 2, ccbor_tag_serializable);

/******************************************************************************/
// Module

// Module attributes that are not constants: the registry dicts (as umsgpack
// exposes them) and ALoader, which is Python code in ccbor_aio.py.
static mp_obj_t ccbor___getattr__(mp_obj_t attr) {
    qstr name = mp_obj_str_get_qstr(attr);
    if (name == MP_QSTR_custom || name == MP_QSTR_builtins || name == MP_QSTR_tags) {
        return ext_dict(ext_registry(true), name);
    }
    if (name == MP_QSTR_ALoader) {
        mp_obj_t mod = mp_import_name(MP_QSTR_ccbor_aio, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
        return mp_load_attr(mod, MP_QSTR_ALoader);
    }
    return MP_OBJ_NULL;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ccbor___getattr___obj, ccbor___getattr__);

static MP_DEFINE_STR_OBJ(ccbor_version_obj, "2.0.0");

static const mp_rom_map_elem_t ccbor_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ccbor) },
    { MP_ROM_QSTR(MP_QSTR___version__), MP_ROM_PTR(&ccbor_version_obj) },
    { MP_ROM_QSTR(MP_QSTR___getattr__), MP_ROM_PTR(&ccbor___getattr___obj) },
    { MP_ROM_QSTR(MP_QSTR_dumps), MP_ROM_PTR(&ccbor_dumps_obj) },
    { MP_ROM_QSTR(MP_QSTR_dump), MP_ROM_PTR(&ccbor_dump_obj) },
    { MP_ROM_QSTR(MP_QSTR_loads), MP_ROM_PTR(&ccbor_loads_obj) },
    { MP_ROM_QSTR(MP_QSTR_load), MP_ROM_PTR(&ccbor_load_obj) },
    { MP_ROM_QSTR(MP_QSTR_Unpacker), MP_ROM_PTR(&ccbor_unpacker_type) },
    { MP_ROM_QSTR(MP_QSTR_Tag), MP_ROM_PTR(&ccbor_tag_type) },
    { MP_ROM_QSTR(MP_QSTR_tag_serializable), MP_ROM_PTR(&ccbor_tag_serializable_obj) },
    { MP_ROM_QSTR(MP_QSTR_float_precision), MP_ROM_QSTR(CCBOR_FLOAT_PRECISION) },
    { MP_ROM_QSTR(MP_QSTR_PackException), MP_ROM_PTR(&mp_type_ccbor_PackException) },
    { MP_ROM_QSTR(MP_QSTR_UnpackException), MP_ROM_PTR(&mp_type_ccbor_UnpackException) },
    { MP_ROM_QSTR(MP_QSTR_UnsupportedTypeException), MP_ROM_PTR(&mp_type_ccbor_UnsupportedTypeException) },
    { MP_ROM_QSTR(MP_QSTR_InsufficientDataException), MP_ROM_PTR(&mp_type_ccbor_InsufficientDataException) },
    { MP_ROM_QSTR(MP_QSTR_InvalidStringException), MP_ROM_PTR(&mp_type_ccbor_InvalidStringException) },
    { MP_ROM_QSTR(MP_QSTR_InvalidCodeException), MP_ROM_PTR(&mp_type_ccbor_InvalidCodeException) },
    { MP_ROM_QSTR(MP_QSTR_UnhashableKeyException), MP_ROM_PTR(&mp_type_ccbor_UnhashableKeyException) },
    { MP_ROM_QSTR(MP_QSTR_DuplicateKeyException), MP_ROM_PTR(&mp_type_ccbor_DuplicateKeyException) },
};
static MP_DEFINE_CONST_DICT(ccbor_module_globals, ccbor_module_globals_table);

const mp_obj_module_t mp_module_ccbor = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ccbor_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ccbor, mp_module_ccbor);
