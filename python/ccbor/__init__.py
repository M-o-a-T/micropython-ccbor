# ccbor: the Python face of the C CBOR codec (_ccbor).
#
# Part of ccbor. MIT License, Copyright (c) 2026 Carlos Tangerino.
#
# The C module _ccbor does the packing and unpacking; this package holds
# everything that is not in the critical path: the tag registry
# (custom / tags), register() and the tag_serializable() decorator, and
# (lazily, on first use) ALoader. The C core reads the registry dicts
# from this module's globals at every call, so registration is effective
# immediately and a soft reset discards the registry together with the
# module.

from _ccbor import dumps
from _ccbor import dump
from _ccbor import loads
from _ccbor import load
from _ccbor import Unpacker
from _ccbor import Tag
from _ccbor import float_precision
from _ccbor import PackException
from _ccbor import UnpackException
from _ccbor import UnsupportedTypeException
from _ccbor import InsufficientDataException
from _ccbor import InvalidStringException
from _ccbor import InvalidCodeException
from _ccbor import UnhashableKeyException
from _ccbor import DuplicateKeyException
from _ccbor import __version__

__all__ = [
    "ALoader",
    "DuplicateKeyException",
    "InsufficientDataException",
    "InvalidCodeException",
    "InvalidStringException",
    "PackException",
    "Tag",
    "UnhashableKeyException",
    "UnpackException",
    "Unpacker",
    "UnsupportedTypeException",
    "__version__",
    "custom",
    "dump",
    "dumps",
    "float_precision",
    "load",
    "loads",
    "register",
    "tag_serializable",
    "tags",
]

#: {class: (tag number, pack callable)} - the pack-side registry.
custom = {}

#: {tag number: unpack callable or None} - the decode-side registry.
tags = {}


def register(cls, tag, pack=None, unpack=None):
    """Register @cls (any class: user-defined or built-in) for a CBOR tag.

    ``pack(obj, options)`` is called when an instance of the class (or of a
    subclass) is packed; its result is packed as the tag's content. ``None``
    (the default) means the class is not encodable: packing raises
    UnsupportedTypeException.

    ``unpack(content, options)`` is called when the tag number is decoded;
    its result is the decoded object. ``None`` (the default) means the tag
    is not decodable: decoding it raises NotImplementedError.

    Tags 2 and 3 are the native bignum tags and cannot be registered.
    """
    if not isinstance(tag, int):
        raise TypeError("Tag number is not type integer")
    if tag < 0:
        raise ValueError("Tag number " + str(tag) + " is negative")
    if (tag >> 64) != 0:
        raise ValueError("Tag number " + str(tag) + " is out of range of 0 to 2**64-1")
    if tag == 2 or tag == 3:
        raise ValueError("tag 2 and 3 are the native bignum tags")
    if pack is not None and not callable(pack):
        raise TypeError("pack must be callable")
    if unpack is not None and not callable(unpack):
        raise TypeError("unpack must be callable")
    if tag in tags:
        raise ValueError("Tag number " + str(tag) + " is already registered")
    if cls in custom:
        raise ValueError("Class " + repr(cls) + " already registered with tag number " + str(tag))
    tags[tag] = unpack
    custom[cls] = (tag, pack)


def tag_serializable(tag_number, example=None):
    """Class decorator that registers a tag, umsgpack style.

    Without ``example``: the class's ``packb(self)`` packs an instance and
    its ``unpackb(data, options)`` (a static or class method) decodes the
    tag. Without ``packb`` the class registers as not encodable, without
    ``unpackb`` as not decodable.

    With ``example``: a built-in type is registered instead, and the
    class's ``packb(obj, options)`` / ``unpackb(data, options)`` are its
    codecs (static methods).
    """

    def deco(cls):
        if example is None:
            pack = getattr(cls, "packb", None)
            if pack is not None:

                def pack_(obj, options, _pack=pack):
                    return _pack(obj)

                pack = pack_
            register(cls, tag_number, pack, getattr(cls, "unpackb", None))
        else:
            register(
                example, tag_number, getattr(cls, "packb", None), getattr(cls, "unpackb", None)
            )
        return cls

    return deco


def __getattr__(attr):
    # ALoader needs asyncio; import it only on first use.
    if attr == "ALoader":
        from .aio import ALoader

        return ALoader
    raise AttributeError(attr)
