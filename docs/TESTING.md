# How ccbor is tested

A serializer is trusted with every message an application sends and
receives, and on a microcontroller a bug in C does not raise an exception: it
reboots the board, or worse. So `ccbor` is tested from several independent
angles, each able to catch what the others miss.

- [Summary](#summary)
- [Running the tests](#running-the-tests)
- [1. The run-tests.py suite](#1-the-run-testspy-suite)
- [2. The refcbor oracle gate](#2-the-refcbor-oracle-gate)
- [3. Differential test against refcbor](#3-differential-test-against-refcbor)
- [4. Conformance: cbor-test-vectors](#4-conformance-cbor-test-vectors)
- [5. Interoperability with cbor2](#5-interoperability-with-cbor2)
- [6. Fuzzing under sanitizers](#6-fuzzing-under-sanitizers)
- [7. Coverage](#7-coverage)
- [8. Memory](#8-memory)
- [9. Real hardware](#9-real-hardware)
- [Build configurations](#build-configurations)
- [Benchmarks](#benchmarks)
- [Problems found along the way](#problems-found-along-the-way)

## Summary

Results of the CBOR rewrite (2026-10-31), all passing:

| test | unix port | boards |
|---|---|---|
| [run-tests.py suite](#1-the-run-testspy-suite) (13 files, ~380 cases) | standard, float32, sanitize | to be re-measured (no board in this cycle) |
| [refcbor oracle gate](#2-the-refcbor-oracle-gate) | 2000 objects + appendix A vectors, byte-identical with cbor2 | (host only) |
| [differential vs refcbor](#3-differential-test-against-refcbor) | ~1600 checks per quick run, several seeds | to be re-measured |
| [cbor-test-vectors](#4-conformance-cbor-test-vectors) | 1998 checks, 0 fails, 80 accounted skips (1381 vector cases) | to be re-measured |
| [interoperability with cbor2](#5-interoperability-with-cbor2) | 6000 objects both ways, identical canonical encodings | (host only) |
| [fuzzing](#6-fuzzing-under-sanitizers) | ASan + UBSan: 10,000 iterations per run | to be re-measured |
| [coverage](#7-coverage) | see `make coverage` | |
| [memory](#8-memory) | no leak; bounded allocation | to be re-measured |

The board columns of the 1.0.0 (MessagePack) release are in its CHANGELOG;
the ESP32/ESP32-P4 figures will be re-measured with hardware in the next
cycle and the tables in [BENCHMARKS.md](BENCHMARKS.md) extended then.

## Running the tests

| command | what | time |
|---|---|---|
| `make test-quick` | everything below on the unix port, smaller | ~2 min |
| `make test` | everything below on the unix port (standard and float32 builds) | ~15 min |
| `make test-sanitize` | the same on an ASan + UBSan build | ~8 min |
| `make coverage` | the suites on the coverage build, then the coverage report | ~2 min |
| `make test-device PORT=...` | suite, differential, conformance and fuzzer on a board | ~15 min |
| `tools/docker-ci.sh` | the unix jobs of the CI, in an Ubuntu container | slow |

The host tests are driven by `pytest` (`tests/host/`); each one runs a
MicroPython program on a unix build of the module. Select the builds with
`CCBOR_FLAVOURS="standard float32"` and the short versions with
`CCBOR_QUICK=1`.

## 1. The run-tests.py suite

`tests/mp/` holds the test programs in MicroPython's own format: a script and
its expected output, run by MicroPython's `run-tests.py`. They cover, per
file: `ccbor_pack` (every RFC 8949 format at every size boundary, the
appendix A vectors), `ccbor_unpack` (every format on the way in, including
non-minimal and indefinite-length encodings, the options), `ccbor_tag` (the
decorator, user and built-in classes, the Tag wrapper), `ccbor_unpacker`
(incremental decoding, chunk by chunk), `ccbor_safety` (hostile input, deep
nesting, containers mutated from Python), `ccbor_stream`, `ccbor_int64` (the
64-bit limits, bignums), `ccbor_large` (the wide length headers), `ccbor_memory`,
`ccbor_errors` (every exception's type and message), `ccbor_subclass`,
`ccbor_edge`, `ccbor_aloader` (asyncio, where available).

## 2. The refcbor oracle gate

[`refcbor`](../tests/diff/refcbor.py) is the pure-Python CBOR codec used as
the differential reference. Before it can be trusted as an oracle it must
agree with [cbor2](https://github.com/agronholm/cbor2), the reference
implementation for CPython: `tests/host/test_refcbor.py` runs it on the host
and checks byte-identical canonical packing on thousands of random objects,
the RFC 8949 appendix A vectors, and decode agreement both ways. Only after
this gate does the differential test use it.

## 3. Differential test against refcbor

`tests/diff/diff_ccbor.py` runs under MicroPython with both modules
importable. Random objects are packed by both and must give the same bytes
or the same exception (name and message); random, valid, mutated and
truncated byte strings are unpacked by both and must give equal objects
(up to the Tag class and float width) or the same exception. The PRNG is
seeded, so a run is reproducible; several seeds ran during development.

This test found a real bug in the C module: a BREAK byte inside a *definite*
container nested in an indefinite one leaked as a sentinel item
(`9f 91 ff ...` silently ended a 17-item array).

## 4. Conformance: cbor-test-vectors

[cbor-test-vectors](https://github.com/cbor-wg/cbor-test-vectors) is the
CBOR working group's corpus. We read its **`.cbor` sidecars** — the repo's
own README recommends them if you don't want an EDN parser — converting
them on the host with cbor2 (`tests/conformance/edn2json.py`, which fails
loudly on anything unexpected) into JSON for the MicroPython driver
(`tests/conformance/conformance.py`).

The driver checks every case: decode must give the expected value (tags as
`Tag`, undefined as `Ellipsis`, floats by exact bits, NaN
payload-insensitively); the `bad.cbor` inputs must all raise a ccbor
exception; and where the vector expects a round trip, `dumps(value,
canonical=True)` must give the vector's bytes.

Every skip is counted and printed with its reason: encodeOptions the
vectors carry that we would not choose (`avoidInts`, `dateTag`), NaN
payloads (we pack the canonical `f97e00`), unassigned simple values (by
design, D3), nesting beyond our 32-level limit, the two date cases that only
fail for cbor2 because it converts them to `datetime`, one UTF-8 case where
MicroPython's own checker accepts the overlong form, and one map that uses
`0` and `False` as distinct keys (impossible in a MicroPython dict). The
skip count is exact: 80 on the standard build, more on float32 (doubles
that do not survive single precision skip re-encode; decode is still
checked).

## 5. Interoperability with cbor2

[cbor2](https://github.com/agronholm/cbor2) is the reference implementation
for CPython. `tests/host/test_interop.py`:

- packs thousands of random objects with cbor2, unpacks and re-packs them
  with ccbor (canonical), and unpacks the result with cbor2: the objects
  must survive both directions;
- checks that for the same value ccbor and cbor2 choose the same canonical
  bytes, byte for byte, at every size boundary (integers, strings, bytes,
  containers, floats including half-exact values and NaN);
- checks `Tag` <-> `CBORTag` and `Ellipsis` <-> `undefined`.

The tests run against the Debian-packaged cbor2 6.1.2 (the Rust
implementation); nothing depends on `cbor2.__version__` (a stale dist-info
may report 5.9.0).

## 6. Fuzzing under sanitizers

`tests/fuzz/fuzz_ccbor.py` runs on the sanitizer build, where any memory
error aborts the process:

1. Random, mutated and truncated byte strings go through `loads()`,
   `load()` and an `Unpacker` fed in random chunks: only ccbor exceptions
   (and `RuntimeError` for nesting) may come out, never a crash, and the
   three entry points must agree. The mutation set inserts CBOR-relevant
   bytes (bad additional info, indefinite lengths, BREAK, bignum tags,
   one-byte simple values).
2. Random objects round-trip: `loads(dumps(o))` equals `o`, up to what CBOR
   cannot distinguish (tuple/list, bytearray/bytes) and the smallest-exact
   float width (compared re-packed).

## 7. Coverage

`make coverage` builds the coverage variant and reports line and branch
coverage of the module; the suites above drive nearly every path. The
8-byte length headers beyond 32 bits are the known gap (they need objects
bigger than the heap).

## 8. Memory

`ccbor_memory.py` (in the suite) checks that repeated packing and unpacking
does not leak, and that the heap used by one call is bounded by the size of
the data.

## 9. Real hardware

The suite, the differential test, the conformance check and the fuzzer run
on real boards with `make test-device`, using the test firmware described
in [HARDWARE_TESTING.md](HARDWARE_TESTING.md). The board results of this
CBOR cycle are pending (no board in this cycle); the 1.0.0 MessagePack
release ran everything on ESP32 and ESP32-P4.

## Build configurations

| build | where | floats | ints | notes |
|---|---|---|---|---|
| unix `standard` | Linux | double | arbitrary precision | 64-bit |
| unix `float32` | Linux | single | arbitrary precision | the float semantics of the boards |
| unix `longlong` | Linux (CI) | double | 64-bit only | a 32-bit build; bignums wrap in `Tag` |
| unix `minimal` | Linux (CI) | none | | build check of the smallest configuration |
| unix `sanitize` | Linux | double | arbitrary precision | ASan + UBSan |
| unix `coverage` | Linux | double | arbitrary precision | gcov |
| ESP32 (Xtensa) | ESP32-PICO-V3-02 board | single | arbitrary precision | 1.0.0 ran with and without PSRAM |
| ESP32-P4 (RISC-V) | ESP32-P4 board | single | arbitrary precision | 32 MiB PSRAM |

The CI also builds the firmware for ESP32, ESP32-S3, C3, C6 and P4.

## Benchmarks

`bench/bench.py` measures, for ccbor, refcbor (the pure-Python reference)
and the built-in `json`, on the same payloads: the time of `dumps` and
`loads` (median of 5 runs, each long enough for the timer), the heap
allocated by one call (with the GC disabled) and the size of the result.
`tools/bench.py` runs it on unix builds and boards and writes
[BENCHMARKS.md](BENCHMARKS.md).

## Problems found along the way

Testing found real problems during the conversion, among them:

- **BREAK leaked into definite containers** nested inside indefinite ones:
  `9f 91 ff ...` silently ended the 17-item definite array, because the
  sentinel was accepted anywhere inside an indefinite context. Found by
  the differential test; every definite container now rejects it.
- **The BREAK sentinel collided with small int 0** (both are immediate
  values on 64-bit builds): an indefinite container holding a 0 item
  silently ended there — `9f 00 ff` decoded to `[]` instead of `[0]`. Found
  by the conformance corpus; the sentinel is now a static object.
- **`list_to_tuple()` read the sentinel's type field** (ASan
  global-buffer-overflow) when a BREAK came back as a map key; the check now
  happens before any inspection of the decoded item.
- **The scanner mis-measured indefinite strings** (the chunk length of
  `ai < 24` was read as 0) and **half/single floats** (the additional info
  25/26/27 was read as a length). Found by hand while porting the
  conformance corpus.
- **`-2**64` overflowed the magnitude accumulation** in the packer (the
  old msgpack code built a `uint64`); it now encodes as `3b ff..ff` like the
  RFC's vector.
- The 1.0.0 (MessagePack) release's own list is in the CHANGELOG.
