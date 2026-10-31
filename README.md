# ccbor — fast CBOR for MicroPython, in C

[![CI](https://github.com/M-o-a-T/micropython-ccbor/actions/workflows/ci.yml/badge.svg)](https://github.com/M-o-a-T/micropython-ccbor/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/M-o-a-T/micropython-ccbor)](https://github.com/M-o-a-T/micropython-ccbor/releases/latest)
[![MicroPython](https://img.shields.io/badge/MicroPython-v1.29-2b2728)](https://micropython.org)
[![ESP32 | S3 | C3 | C6 | P4](https://img.shields.io/badge/ESP32-S3%20%7C%20C3%20%7C%20C6%20%7C%20P4-e7352c)](https://github.com/M-o-a-T/micropython-ccbor/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

`ccbor` is a [CBOR (RFC 8949)](https://www.rfc-editor.org/rfc/rfc8949)
serializer for [MicroPython](https://micropython.org), written in C as a
user module. It began as `msgpackc`, a MessagePack accelerator with the API
of Peter Hinch's excellent pure-Python
[umsgpack](https://github.com/peterhinch/micropython-msgpack), and was
converted to CBOR wholesale: the same `dumps`/`loads`/streams shape, but
speaking CBOR — tags instead of extension types, half floats, bignums,
indefinite lengths on the way in — tens of times faster than any pure-Python
codec and allocating a fraction of the memory.

```python
import ccbor

data = ccbor.dumps({"id": "sensor-01", "temp": 21.5, "ok": True, "raw": b"\x01\x02"})
obj = ccbor.loads(data)
```

CBOR is like JSON, but binary: smaller messages, a `bytes` type, 64-bit
integers, and no text to parse. A `dict` that takes 60 bytes as JSON takes
33 as CBOR, and any CBOR library in any language reads it. The output is
deterministic (RFC 8949 preferred encodings), and byte-identical with
[cbor2](https://github.com/agronholm/cbor2)'s canonical form.

<!-- BENCH-SUMMARY -->

Time to pack and unpack a list of 20 telemetry records (1 KiB), and heap allocated, per call ([all the figures](docs/BENCHMARKS.md)):

| | ccbor dumps | ccbor-ref dumps | | ccbor loads | ccbor-ref loads | |
|---|--:|--:|--:|--:|--:|--:|
| unix port, Apple Silicon | **4.0 µs**, 3680 B | 214 µs, 50848 B | **53.5x** faster | **15.4 µs**, 5312 B | 253 µs, 56896 B | **16.4x** faster |

<!-- /BENCH-SUMMARY -->

## Features

- `dumps`, `dump`, `loads`, `load`, with the options of the umsgpack API
  (`force_float_precision`, `use_tuple`, `use_ordered_dict`,
  `allow_invalid_utf8`) plus `canonical=` for cbor2-compatible key order.
- **CBOR tags** with the `@tag_serializable` decorator, for your classes and
  for built-in types such as `set` or `complex`: the content is a CBOR item,
  not raw bytes. Unregistered tags decode to a `Tag` wrapper (compatible
  with moat's `Tag` and cbor2's `CBORTag`).
- **Full CBOR on the way in**: non-minimal encodings, indefinite-length
  strings, arrays and maps; bignum tags 2/3 to `int`; half floats expanded;
  `undefined` as `Ellipsis`.
- **Streams**: `load` reads exactly one object from a file, socket or UART;
  `dump` writes it in one call.
- **Incremental decoding** with `Unpacker`, for data that arrives in pieces,
  and **asyncio** decoding with `ALoader`.
- **Safe on untrusted data**: corrupt or hostile input raises an exception;
  it never crashes the board nor allocates memory it does not need. Tested
  with fuzzing under AddressSanitizer and UndefinedBehaviorSanitizer.
- Tested on the unix port; conforms to the cross-language
  [cbor-test-vectors](https://github.com/cbor-wg/cbor-test-vectors) (1381
  cases) and interoperates with [cbor2](https://github.com/agronholm/cbor2),
  both directions, byte for byte in canonical form. The 1.0.0 MessagePack
  release ran its whole suite on real ESP32 and ESP32-P4 boards; the board
  pass of the CBOR edition is pending hardware.
- About 12 KiB of flash (15 KiB on RISC-V chips), no RAM reserved, no configuration.

## Getting started

**Just want to try it on an ESP32?** Download a ready-made firmware (ESP32,
ESP32 with PSRAM, S3, C3, C6, P4) from the
[latest release](https://github.com/M-o-a-T/micropython-ccbor/releases/latest)
and write it with `esptool.py`: no compiler needed. The release page tells
which file and address to use for your chip.

**Never built MicroPython?** Follow [docs/INSTALL.md](docs/INSTALL.md): it
goes step by step from a fresh computer to a board running `ccbor`. In
short:

```bash
git clone --recursive https://github.com/M-o-a-T/micropython-ccbor.git
cd micropython-ccbor
make setup                     # tools for your computer
make unix                      # MicroPython for your computer, with ccbor
build/unix-standard/micropython -c "import ccbor; print(ccbor.dumps([1, 2, 3]))"

make esp-idf                   # the ESP32 toolchain, installed inside this directory
make firmware BOARD=ESP32_GENERIC
make deploy BOARD=ESP32_GENERIC PORT=/dev/ttyUSB0   # erases the board!
```

**Already build your own firmware?** Add the module to it:

```bash
make USER_C_MODULES=/path/to/micropython-ccbor/cmodules/micropython.cmake   # esp32, rp2
make USER_C_MODULES=/path/to/micropython-ccbor/cmodules                     # unix, stm32, ...
```


## Documentation

| | |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | building and flashing, for beginners |
| [docs/USAGE.md](docs/USAGE.md) | the API, with examples |
| [docs/TESTING.md](docs/TESTING.md) | how ccbor is tested, and the results |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | speed and memory on the unix port (board figures pending hardware) |
| [docs/HARDWARE_TESTING.md](docs/HARDWARE_TESTING.md) | testing on a board without losing what is on it |
| [CONTRIBUTING.md](CONTRIBUTING.md) | how to contribute |

## Repository layout

```
cmodules/                     the module: all you need to add it to a firmware
  micropython.cmake           entry point for USER_C_MODULES (CMake ports)
  ccbor/modccbor.c            the code
python/
  ccbor_aio.py                ccbor.ALoader (asyncio)
tests/
  mp/                         the run-tests.py suite
  diff/                       refcbor (the pure-Python reference) and the
                              differential test against it
  conformance/                the cbor-test-vectors driver and converter
  fuzz/                       the fuzzer (for the sanitizer build)
  host/                       the pytest harness (interop, oracle gate, ...)
firmware/                     manifests of the firmware built here
bench/bench.py                the benchmark
tools/                        build, flash, test and benchmark scripts
micropython/                  MicroPython v1.29.0 (git submodule)
third_party/                  cbor-test-vectors, for the tests (submodule)
```

The ESP-IDF and the Espressif toolchains are installed in `tools/esp-idf`
and `tools/espressif` by `make esp-idf`; like `build/`, they are not part of
the repository.

## Acknowledgements

This project stands on the work of others, with gratitude:

- **Peter Hinch**, for [micropython-msgpack](https://github.com/peterhinch/micropython-msgpack)
  (umsgpack). Its careful API, its extension mechanism and its asyncio
  loader are the design `ccbor` follows, its test suite checks
  `ccbor` today, and its behaviour is the reference of our differential
  tests. If you prefer pure Python, or need to run on a stock firmware, use
  umsgpack: it is excellent. Peter's many other MicroPython libraries and
  documents are a treasure for the community.
- **Vanya A. Sergeev**, for [u-msgpack-python](https://github.com/vsergeev/u-msgpack-python),
  from which umsgpack was ported, and **WZab**, whose first port to
  MicroPython started it.
- **Damien George and the MicroPython contributors**, for
  [MicroPython](https://github.com/micropython/micropython).
- **Yusuke Kawasaki**, for the [msgpack-test-suite](https://github.com/kawanet/msgpack-test-suite)
  (the conformance corpus of the 1.0.0 MessagePack release).
- **Sadayuki Furuhashi** and the MessagePack community, for the
  [format](https://github.com/msgpack/msgpack/blob/master/spec.md).
- **C. Bormann** and the CBOR WG, for [RFC 8949](https://www.rfc-editor.org/rfc/rfc8949)
  and the [cbor-test-vectors](https://github.com/cbor-wg/cbor-test-vectors)
  corpus.
- **Alex Grönholm**, for [cbor2](https://github.com/agronholm/cbor2), the
  reference the tests interoperate with.

`ccbor` began as the MessagePack encoder written in C for the firmware of
the coIoTe IoT devices, as an accelerator of their Python packer; it was then
extracted, completed with a decoder, streams and extension types, and made a
general purpose module.

## License

MIT, see [LICENSE](LICENSE). The test submodules are MIT licensed by
their authors.
