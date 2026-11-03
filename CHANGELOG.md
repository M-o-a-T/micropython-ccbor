# Changelog

All notable changes to this project. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses [Semantic Versioning](https://semver.org/).

## [2.1.0] - 2026-11-03

The Python-package edition: the C module is now `_ccbor`, and `ccbor` is a
small Python package around it that owns the tag registry. The primary API
(`dumps`/`dump`/`loads`/`load`, `Unpacker`, `Tag`, `tag_serializable`, the
options, the exceptions) is unchanged.

**Added**
- `ccbor.register(cls, tag, pack=None, unpack=None)`: register **any**
  class — built-in types included, no wrapper class needed — with plain
  `pack(obj, options)` / `unpack(content, options)` callables.
  `pack=None` keeps the native encoding (or means "not encodable"),
  `unpack=None` means "not decodable" (`NotImplementedError`), the shape
  for decoder-only tags.
- The pack-side tag lookup follows the class hierarchy (the MRO): one
  dictionary hit on the type, then the bases — instead of scanning the
  whole registry with `isinstance` for every packed object.
- The `ccbor` package: `register()`, the registry (`ccbor.custom`,
  `ccbor.tags`), `tag_serializable()` and `ccbor.ALoader` (lazily
  imported from `ccbor/aio.py`) are Python now; the C core keeps only the
  critical path.
- `memoryview` packs as a byte string, like `bytes` and `bytearray`.
- The `memory_min` load option: `loads(data, memory_min=N)` unpacks byte
  strings of at least N bytes as `memoryview`s into `data` instead of
  copies — zero-copy decoding of large payloads. `load()` and `Unpacker`
  reject the option (a stream has no buffer to refer to, and `feed()`
  moves the Unpacker's buffer); decode `next_raw()`'s bytes with `loads()`
  for the same effect there. On builds without `memoryview` the option
  is silently ignored.
- `Unpacker.feed()` documents what it always did: any read buffer goes
  in — `bytes`, `bytearray`, `memoryview`, sliced or not — and the data
  is copied, so the source may change or go away afterwards.

**Changed**
- The module split: the C module is `_ccbor` (the codec: `dumps`, `dump`,
  `loads`, `load`, `Unpacker`, `Tag`, `float_precision`, the exceptions);
  `ccbor` is the Python package to import, which re-exports all of it.
  A firmware must now freeze the package (`package("ccbor", ...)` in the
  manifest, or copy it to the board) for `import ccbor` to work;
  `import _ccbor` alone has no tag registry and no `ALoader`.
- The registry shapes (they are documented constants): `ccbor.custom` is
  `{class: (tag number, pack callable)}`, `ccbor.tags` is
  `{tag number: unpack callable or None}`. `ccbor.builtins` is removed.
- The registry lives in the `ccbor` module's globals; the hidden
  `_ccbor_ext` module is gone. The C core reads
  `sys.modules["ccbor"].custom/.tags` on every call: registering takes
  effect immediately, a soft reset discards the registry with the module.
- A tag registered without an unpacker raises
  `NotImplementedError: no unpacker for tag N` (was
  `Tag class X lacks unpackb()`; the registry no longer remembers the
  class).
- `tag_serializable(nr, example=atype)` with a wrapper class that has no
  `packb` raises `UnsupportedTypeException` on pack (was
  `NotImplementedError`); `packb` is taken from the class at decoration
  time.
- A duplicate tag number raises `ValueError: Tag number N is already
  registered` (the old message named the class, which the registry no
  longer stores).

**Fixed**
- `tests/diff/diff_ccbor.py` crashed on the float32 build when a random
  map key was a tuple: `canon()` turned it into an unhashable list.

## [c1.0.0] - 2026-10-31

The CBOR edition: the module now speaks CBOR (RFC 8949) instead of
MessagePack, and is renamed `ccbor` (was `msgpackc`). The API keeps the
`dumps`/`dump`/`loads`/`load`/`Unpacker`/`ALoader` shape of umsgpack; the
wire format, the extension mechanism and several options are CBOR's.

**Changed**
- Wire format: CBOR (RFC 8949) — major types 0-7, preferred (minimal)
  encodings, 64-bit length headers.
- `ext_serializable` is now `tag_serializable(tag_number, example=None)`:
  CBOR tags (0..2**64-1) instead of msgpack ext types (-128..127); the
  content is a CBOR item. Tags 2/3 (bignums) are native and cannot be
  registered.
- `ReservedCodeException` is now `InvalidCodeException` (invalid additional
  information, stray BREAK, bad indefinite-string chunks, reserved 1-byte
  simple values).
- Module name `ccbor`, registry module `_ccbor_ext`, registry dict
  `packers` -> `tags`; `__version__` 2.0.0.
- Repository moved to M-o-a-T/micropython-ccbor.

**Added**
- `Tag` wrapper type: unregistered tags decode to it, and instances pack
  generically.
- Bignum tags 2/3 both ways: `int` beyond 64 bits packs as a bignum and
  decodes back, wrapped in `Tag(2/3, bytes)` on 64-bit-int builds.
- Half floats: the packer uses the smallest exact width (f9/fa/fb, like
  cbor2 canonical), `force_float_precision="half"` forces it; NaN packs as
  f97e00 whatever its payload; decoding expands half floats.
- `Ellipsis` <-> `undefined` (f7) both ways.
- `canonical=True` option: sorts map keys like cbor2 (RFC 8949 4.2.3
  length-first order); output byte-identical with cbor2 canonical.
- Indefinite-length decoding: chunked strings, arrays and maps
  (5f/7f/9f/bf ... ff), with full validation; the tag 55799 self-describe
  prefix is stripped on decode, like cbor2.
- Conformance against cbor-wg/cbor-test-vectors (1381 cases, read from
  its .cbor sidecars), differential test against the in-repo pure-Python
  refcbor (validated against cbor2 first), interop tests against cbor2,
  and the fuzzer rewritten for CBOR.

**Removed**
- `umsgpack` compatibility package (nothing to be compatible with).
- `python/msgpackc_aio.py` is now `python/ccbor_aio.py`.

**Fixed** (found by the new tests during the conversion)
- BREAK bytes no longer leak into definite containers nested inside
  indefinite ones (a 17-item array could silently end at an inner ff).
- The BREAK sentinel no longer collides with small int 0 (9f 00 ff decoded
  to [] instead of [0] on 64-bit builds).
- `-2**64` encodes as 3b ff..ff (the magnitude accumulation overflowed).

## [1.0.0] - 2026-10-18

First release.

- C module `msgpackc` for MicroPython v1.29: `dumps`, `dump`, `loads`,
  `load`, with umsgpack's options and exceptions and byte-identical output.
- `ext_serializable` decorator for user classes and built-in types,
  compatible with umsgpack's extension modules.
- `Unpacker`: incremental decoding with `feed()`, `next_raw()`, `reset()`
  and `max_buffer_size`.
- `ALoader` (Python, `python/msgpackc_aio.py`): asyncio decoding.
- `umsgpack` compatibility package (`python/umsgpack/`).
- Nesting limited to 32 levels (`CCBOR_MAX_DEPTH`).
- Build, flash, test and benchmark tooling for the unix port and ESP32
  boards, with a repo-local ESP-IDF v5.5.2.
- Ready-made firmware in each GitHub release, built by CI
  (`.github/workflows/release.yml`).
