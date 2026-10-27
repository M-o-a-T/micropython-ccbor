# ccbor (the C module)

This directory is the whole module: copy it (with `../micropython.cmake`) to
add `ccbor` to any MicroPython v1.29+ firmware.

```bash
# CMake ports (esp32, rp2)
make USER_C_MODULES=/path/to/cmodules/micropython.cmake
# make ports (unix, stm32, samd, nrf, mimxrt...)
make USER_C_MODULES=/path/to/cmodules
```

- `modccbor.c`: the module, one file, no dependency besides MicroPython.
- `micropython.cmake`, `micropython.mk`: its build rules.

Optional: `-DCCBOR_MAX_DEPTH=n` changes the nesting limit (default 32).

Documentation: [../../docs/USAGE.md](../../docs/USAGE.md).
License: MIT, see [../../LICENSE](../../LICENSE).
