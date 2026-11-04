# Changelog

All notable changes to this project. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses [Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-11-03

The CBOR edition: the module now speaks CBOR (RFC 8949) instead of
MessagePack, and is renamed `ccbor` (was `msgpackc`). The API keeps the
`dumps`/`dump`/`loads`/`load`/`Unpacker`/`ALoader` shape of umsgpack; the
wire format, the extension mechanism, and several options are CBOR's.

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

# `msgpackc` changelog

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
- Nesting limited to 32 levels (`MSGPACKC_MAX_DEPTH`).
- Build, flash, test and benchmark tooling for the unix port and ESP32
  boards, with a repo-local ESP-IDF v5.5.2.
- Ready-made firmware in each GitHub release, built by CI
  (`.github/workflows/release.yml`).
