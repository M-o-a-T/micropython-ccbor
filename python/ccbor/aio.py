# ccbor/aio.py: asynchronous CBOR decoding for ccbor.
#
# Part of ccbor. MIT License, Copyright (c) 2026 Carlos Tangerino.
#
# The ALoader API follows Peter Hinch's umsgpack.ALoader
# (https://github.com/peterhinch/micropython-msgpack), implemented on
# top of the C ccbor.Unpacker: it decodes the CBOR objects arriving on an
# asyncio StreamReader. It is normally reached as ccbor.ALoader: the
# package imports this module on first use.

import ccbor


class ALoader:
    """Decode the CBOR objects arriving on an asyncio StreamReader.

    loader = ALoader(asyncio.StreamReader(uart), **load_options)
    async for obj in loader:
        ...
    """

    CHUNK = 128  # bytes asked for by each read()

    def __init__(self, fp, **options):
        self._fp = fp
        # Called with the raw bytes of each decoded object, then with b"".
        self._observer = options.pop("observer", None)
        self._options = options
        self._unpacker = ccbor.Unpacker(**options)

    def __aiter__(self):
        return self

    async def __anext__(self):
        try:
            return await self.load()
        except EOFError:
            if len(self._unpacker):
                raise  # the stream ended in the middle of an object
            raise StopAsyncIteration

    async def load(self):
        """Return the next object. Raises EOFError at the end of the stream."""
        u = self._unpacker
        while True:
            if self._observer is None:
                for obj in u:
                    return obj
            else:
                raw = u.next_raw()
                if raw is not None:
                    self._observer(raw)
                    self._observer(b"")
                    return ccbor.loads(raw, **self._options)
            data = await self._fp.read(self.CHUNK)
            if not data:
                raise EOFError
            u.feed(data)
