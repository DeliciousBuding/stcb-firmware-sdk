# stcb-firmware-sdk

**Board-level firmware / hardware abstraction SDK for the STC-B learning board (IAP15F2K61S2).**
A reusable, open-source lower layer shared by device firmware, course projects, and the
`cloud-path-driver-stcb` platform device driver.

## Layout

| Directory | Purpose |
|-----------|---------|
| `BSP/` | Board support package (`inc/*.h` API + `STCBSP_V3.6.LIB` Keil C51 static library) |
| `tools/` | Device tooling: `serlink.py` (serial base / `STC_PORT` / NUL scrubbing) · `stcflash.py` (flashing SSOT) · `serial-console.py` · `serial-log.py` |
| `examples/sensor-probe/` | Legacy 2 KB bring-up probe (V/B/L/N/T) |
| `examples/rtc-battery-probe/` | DS1302 battery-retention ablation probe (including board-specific evidence) |
| `examples/iap-probe/` | IAP self-programming probe (single-sector hardware evidence; OTA is not implemented) |
| `examples/stcb-full/` | CloudPath reference firmware: full board capabilities, Device Protocol v1, real ACK/ERROR |

## Usage

- Firmware build/flash currently targets Windows with Keil C51 (`C51/BL51/OH51`); the Python serial tools are portable.
- `KEIL_HOME` overrides the default `C:\Keil_v5` install; serial defaults to `COM3` and can be overridden with `STC_PORT`.
- Python dependencies: `python -m pip install pyserial stcgal`.
- Build the full firmware: `cd examples/stcb-full && python build.py` (`--flash` delegates to `tools/stcflash.py`; press board Reset if software reset is unavailable).
- Every `build.py` locates the toolchain through `KEIL_HOME`; no machine-specific source edits are required.

## Ecosystem

Companion to [CloudPath](https://github.com/DeliciousBuding/cloud-path) — a cloud-native, plugin-driven IoT
control platform. The chain is **board → SDK → cloud**: device firmware (this SDK) → platform device driver
(`cloud-path-driver-stcb`) → control platform, forming an edge-cloud synergy pipeline.

## Open-source scope

- Includes: **interface headers + BSP static library + tooling + reference firmware** (interface-level open source; linkable & extendable).
- Source-level open source would require the BSP `.c` sources (not currently included).
- License: Apache License 2.0 (see `LICENSE`). BSP/teaching-material attribution and third-party boundaries are recorded in `NOTICE`.
