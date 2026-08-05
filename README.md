# ButteRFly, a WHAD-compatible firmware for nRF52-based USB dongles

[![C/C++ compilation CI](https://github.com/whad-team/butterfly/actions/workflows/compile.yml/badge.svg)](https://github.com/whad-team/butterfly/actions/workflows/compile.yml)

## Introduction

ButteRFly is a firmware specifically designed to be used with [WHAD](https://github.com/whad-team/whad-client)
that provides the following features:

- Bluetooth Low Energy connection sniffing, scanning, hijacking and PDU injection
- ZigBee sniffing, scanning and packet injection
- Nordic Semiconductor's Enhanced ShockBurst protocol sniffing, scanning and packet injection
- Logitech Unifying sniffing, scanning and packet injection

## Installation 

Follow [WHAD's documentation instructions](https://whad.readthedocs.io/en/stable/device/compat.html#makerdiary-nrf52840-mdk-usb-dongle) to install this firmware on a Makerdiary nRF52 MDK USB dongle
or a Nordic's nRF52 dongle.

## Reference

ButteRFly has been initially released as a proof-of-concept by Romain Cayre in his presentation
at IEEE/IFIP International Conference on Dependable Systems and Networks (DSN), Jun 2021, Taipei (virtual), Taiwan.
The related paper is [available here](https://laas.hal.science/hal-03193297v2/file/injectable_final_version.pdf).

---

## What's different on the `clue` branch

This is the `XTheocharis/butterfly` fork (branch `clue`) tracking `upstream/whad-team/butterfly#main`. The branch adds a complete **Adafruit CLUE (nRF52840)** platform on top of the existing PCA10059 and MDK-DONGLE targets, gated behind `BOARD_CLUE` so existing builds are byte-for-byte unchanged.

### CLUE platform
- `config/clue/` — `clue.ld` linker (app origin 0x26000, RAM origin 0x20002260), 7190-line `sdk_config.h` (SoftDevice S140 v6, BLE HIDS, Peer Manager, FDS, CC310 crypto, QSPI, PDM, TWIM1, PWM0/1, SAADC), `custom_board.h` pin map, `check_singletap.sh` post-link guard (rejects single-tap-bypass magic at flash 0x26200).
- `Makefile` — added `BOARD_CLUE` to `SUPPORTED_PLATFORMS`, CLUE-specific CFLAGS/SRC_FILES (~165 lines pulling in SoftDevice + BLE stack + all new subsystems), `clue_post_link_check` target enforcing FLASH ≤ 533299 / RAM ≤ 118784.
- `tools/clue-flash.sh` — pyserial-based DFU entry + UF2 mass-storage copy (shell `printf > /dev/ttyACM*` does NOT work — firmware force-asserts DTR on CDC PORT_OPEN).

### Dual-runtime architecture
- `src/runtime.{cpp,h}` (NEW) — GPREGRET2 one-shot selector (0xC1 raw / 0xC2 BLE), WDT+reset switcher (never in-place SoftDevice teardown).
- `src/platformRuntime.{c,h}` (NEW) — SD-aware SDK wrapper layer (NVIC, GPREGRET, critical region, reset).
- `src/main.cpp` — CLUE-only mode-gated boot: RAW mode runs SVC 0x11 (SoftDevice-disable) + VTOR=0x26000; BLE mode skips both (SD stays alive, VTOR at MBR-default 0x0 so `nrf_sdh_enable_request()` can dispatch SVC). Also calls `timebase_init()`, `pinreg_init()`, `runtime_init()`, `runtime_select()` before Core construction.
- `src/serial.{cpp,h}` — "DFU" magic trigger + DTR force-assert + CDC-close policy (raw resets, BLE survives).

### Board domain (28 commands, all advertised)
- `src/boardModule.{cpp,h}` (NEW 2144L) — dispatcher with all 28 commands advertised and handled: sensors, motion, storage (QSPI adopt/read/erase), expert I/O (GPIO/I2C/SPI/ADC), audio (PDM raw PCM), outputs (buzzer/NeoPixel/LED), runtime config, BLE-HID pairing/bonds/profiles.
- `src/capabilities.h` — `CMD()` macro fix (`1ULL <<` to avoid signed-int UB), `CAPABILITIES_RAW_WHAD[]` vs `CAPABILITIES_BLE_HID[]` tables, `getRuntimeCapabilities(mode)` selector.

### New subsystems (all gated by `BOARD_CLUE`)
- `src/sensors/` — IMU (LSM6DS33), magnetometer (LIS3MDL), APDS9960 (gesture+color+proximity), BMP280, SHT31-D
- `src/motion/` — Madgwick AHRS fusion, air mouse, tilt nav, gesture primitives, rotation-gesture confirmation
- `src/storage/` — QSPI NOR flash + 4-partition journal + calib persistence
- `src/expert/` — edge-connector GPIO/I2C/SPI/ADC with pin lease system
- `src/audio/` — PDM microphone
- `src/output/` — PWM1 buzzer
- `src/ble/` — complete BLE-HID runtime (advertising, GATT, LESC security, HIDS, Peer Manager bonds, 4 HID profiles)
- `src/{display,menu,i2cBus,pinRegistry,timebase,messagePool}.{cpp,h}` — ST7789 TFT, recovery menu, I²C bus manager, pin lease registry, 64-bit timebase, fixed-size message pool

### Test infrastructure
- `tests/host/` — 28 GCC/Clang test suites (~12KLoC, 1112 cases, all green). Host-only — refuses `arm-none-eabi-*`. ASan+UBSan via `test-sanitize` target. Deterministic seed (`SEED ?= 0xC0FFEE`). Uses pure-C `*_eval.{c,h}` companion files that mirror each SDK wrapper.

### CI changes
- `.github/workflows/compile.yml` — `wget` → `curl -L --fail` + `sha256sum -c -`, builds all three platforms (PCA10059, MDK_DONGLE, CLUE).
- `.github/workflows/release.yml` — CLUE added to dist + release assets.

### Submodule
- `whad-lib` submodule pinned to `XTheocharis/whad-lib` (branch `clue`) — carries Board domain C API + transport hardening.

See `CLUE-IMPLEMENTATION-PLAN.md` for the authoritative design spec (note the SUPERSEDED DETAILS block at top regarding SDK pin and GPREGRET2 values), and the workspace `README.md` / `TODO.md` for the integrated 4-repo picture.