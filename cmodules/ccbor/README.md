# ccbor (the C core)

This directory is the C half of the module: copy it (with
`../micropython.cmake`) to add the `_ccbor` core to any MicroPython v1.29+
firmware.

```bash
# CMake ports (esp32, rp2)
make USER_C_MODULES=/path/to/cmodules/micropython.cmake
# make ports (unix, stm32, samd, nrf, mimxrt...)
make USER_C_MODULES=/path/to/cmodules
```

- `modccbor.c`: the module, one file, no dependency besides MicroPython.
- `micropython.cmake`, `micropython.mk`: its build rules.

Optional: `-DCCBOR_MAX_DEPTH=n` changes the nesting limit (default 32).

`import ccbor` — the API you normally use, with `register()`, the tag
registry and `ccbor.ALoader` — is the Python package in
[../../python/ccbor](../../python/ccbor): freeze it into the firmware
(`package("ccbor", base_path="/path/to/python")` in the manifest) or copy
it to the board. Without it, `import _ccbor` still gives you the codec
(`dumps`/`dump`/`loads`/`load`, `Unpacker`, `Tag`, the exceptions), but
no tag can be registered and there is no `ALoader`.

Documentation: [../../docs/USAGE.md](../../docs/USAGE.md).
License: MIT, see [../../LICENSE](../../LICENSE).
