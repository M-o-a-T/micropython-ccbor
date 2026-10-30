#!/usr/bin/env python3
"""Convert the cbor-test-vectors .cbor sidecars to plain JSON for the
MicroPython conformance driver.

The vectors repo ships a matching .cbor file next to every .edn; its README
says to use them if you don't want an EDN parser. Each .cbor file decodes
(with cbor2) to a map {title, description, tests: [...]} where each test
has an "encoded" byte string, the "decoded" item, and optional flags:
"roundtrip" (default true), "fail", and per-file/per-test
decodeOptions/encodeOptions.

The JSON form keeps the same shape, with CBOR values mapped to plain JSON
plus small markers the MicroPython driver understands:
  - undefined        -> {"__undefined__": true}
  - CBORSimpleValue  -> {"__simple__": n}
  - CBORTag          -> {"__tag__": n, "value": <mapped content>}
  - float            -> its exact 64-bit hex ("__float__": "7ff0000000000000")
  - byte strings     -> hex ("__bytes__": "0102")
  - bignum ints      -> kept as JSON strings when they exceed 2**53 ("__big__")

Anything unexpected raises: no silent skipping.

Usage: edn2json.py  (writes to stdout)
"""

import json
import struct
import sys
from pathlib import Path

import cbor2

sys.setrecursionlimit(10000)

ROOT = Path(__file__).resolve().parents[2]
VECTORS = ROOT / "third_party" / "cbor-test-vectors" / "tests"


class ConvertError(Exception):
    pass


def map_value(v, depth=0):
    if depth > 900:
        raise ConvertError("deeper than 900 levels")
    if v is None or v is True or v is False:
        return v
    if isinstance(v, int):
        if -(2**53) <= v <= 2**53:
            return v
        return {"__big__": str(v)}
    if isinstance(v, float):
        # exact double bits; NaN keeps its payload for the driver to decide.
        # f32_exact: the value survives single-precision rounding exactly
        # (computed here, on the host, where doubles exist).
        bits = struct.pack(">d", v)
        try:
            f32 = struct.pack(">f", v)
            back = struct.unpack(">f", f32)[0]
            exact32 = struct.pack(">d", back) == bits
        except (OverflowError, ValueError):
            exact32 = False  # inf/huge doubles cannot be held at all
        return {"__float__": bits.hex(), "f32": exact32}
    if isinstance(v, str):
        return v
    if isinstance(v, (bytes, bytearray)):
        return {"__bytes__": bytes(v).hex()}
    if isinstance(v, (list, tuple)):
        return [map_value(x, depth + 1) for x in v]
    if hasattr(v, "items") and not isinstance(v, (str, bytes)):
        # A dict or cbor2's frozendict (map keys decode immutably). CBOR
        # maps can have any item as a key, which a JSON object cannot
        # represent: keep the entries as pairs.
        return {
            "__map__": [[map_value(k, depth + 1), map_value(x, depth + 1)] for k, x in v.items()]
        }
    if type(v) is type(cbor2.undefined):
        return {"__undefined__": True}
    if isinstance(v, cbor2.CBORSimpleValue):
        return {"__simple__": v.value}
    if isinstance(v, cbor2.CBORTag):
        return {"__tag__": v.tag, "value": map_value(v.value, depth + 1)}
    if isinstance(v, RawTag):
        return {"__tag__": v.tag, "value": map_value(v.value, depth + 1)}
    raise ConvertError("unexpected type %r" % type(v))


def map_options(opts):
    if opts is None:
        return None
    if not isinstance(opts, dict):
        raise ConvertError("unexpected options %r" % (opts,))
    out = {}
    for k, v in opts.items():
        if v is True or v is False or isinstance(v, int):
            out[k] = v
        else:
            raise ConvertError("unexpected option value %r=%r" % (k, v))
    return out


class RawTag:
    """A tag wrapper that survives cbor2's semantic decoding."""

    def __init__(self, tag, value):
        self.tag = tag
        self.value = value


def semantic_decoders():
    # cbor2 converts tags 0/1 (dates) to datetime.datetime. The
    # semantic_decoders hook receives the DECODED CONTENT and its return
    # replaces the whole item: wrap the content back with the tag number.
    def make(tagn):
        def fn(content, shareable_index):
            return RawTag(tagn, content)

        return fn

    return {0: make(0), 1: make(1)}


def convert_file(path):
    data = path.read_bytes()
    try:
        doc = cbor2.loads(data, semantic_decoders=semantic_decoders())
    except cbor2.CBORDecodeError as e:
        # good.cbor contains a 200-deep item: retry with a high depth limit
        try:
            doc = cbor2.CBORDecoder(
                __import__("io").BytesIO(data),
                max_depth=1000,
                semantic_decoders=semantic_decoders(),
            ).decode()
        except Exception as e2:
            raise ConvertError("%s: %s / %s" % (path.name, e, e2))
    out = {
        "file": path.name,
        "title": doc.get("title"),
        "tests": [],
    }
    if "decodeOptions" in doc or "encodeOptions" in doc:
        out["decodeOptions"] = map_options(doc.get("decodeOptions"))
        out["encodeOptions"] = map_options(doc.get("encodeOptions"))
    if doc.get("fail"):
        out["fail"] = True
    for i, t in enumerate(doc.get("tests", [])):
        if "encoded" not in t:
            raise ConvertError("%s[%d]: no encoded" % (path.name, i))
        entry = {"i": i, "encoded": t["encoded"].hex()}
        if "description" in t:
            entry["description"] = t["description"]
        if t.get("roundtrip") is False:
            entry["roundtrip"] = False
        if "decodeOptions" in t or "encodeOptions" in t:
            entry["decodeOptions"] = map_options(t.get("decodeOptions"))
            entry["encodeOptions"] = map_options(t.get("encodeOptions"))
        if "decoded" in t:
            entry["decoded"] = map_value(t["decoded"])
        elif not t.get("fail") and not doc.get("fail"):
            raise ConvertError("%s[%d]: no decoded and not a fail case" % (path.name, i))
        out["tests"].append(entry)
    return out


def main():
    files = sorted(VECTORS.rglob("*.cbor"))
    if not files:
        raise ConvertError("no .cbor files under %s" % VECTORS)
    docs = []
    for f in files:
        docs.append(convert_file(f))
    json.dump(docs, sys.stdout, separators=(",", ":"), ensure_ascii=False)
    print()


if __name__ == "__main__":
    main()
