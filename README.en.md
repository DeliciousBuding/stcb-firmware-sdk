# stcb-firmware-sdk

**Board-level firmware / hardware abstraction SDK for the STC-B learning board (IAP15F2K61S2).**
A reusable, open-source lower layer shared by device firmware, course projects, and the
`cloud-path-driver-stcb` platform device driver.

## Layout

| Directory | Purpose |
|-----------|---------|
| `BSP/` | Board support package (`inc/*.h` API + `STCBSP_V3.6.LIB` Keil C51 static library) |
| `tools/` | Device tooling: `serlink.py` (serial base / `STC_PORT` / NUL scrubbing) · `stcflash.py` (flashing SSOT) · `serial-console.py` · `serial-log.py` |
| `examples/sensor-probe/` | Reference probe firmware (all sensors/actuators, within 2KB), command set V/B/L/N/T |

## Usage

- Dependencies: Keil C51 (`C51/BL51/OH51`), serial default `COM3` (override via `STC_PORT`).
- Build the probe: `cd examples/sensor-probe && python build.py` (`--flash` delegates to `tools/stcflash.py`).
- Note: `KEIL_BIN` / `STC_INC` in `build.py` are **machine-specific placeholders**; adjust to your Keil install.

## Ecosystem

Companion to [CloudPath](https://github.com/DeliciousBuding/cloud-path) — a cloud-native, plugin-driven IoT
control platform. The chain is **board → SDK → cloud**: device firmware (this SDK) → platform device driver
(`cloud-path-driver-stcb`) → control platform, forming an edge-cloud synergy pipeline.

## Open-source scope

- Includes: **interface headers + BSP static library + tooling + reference firmware** (interface-level open source; linkable & extendable).
- Source-level open source would require the BSP `.c` sources (not currently included).
- License: MIT (see `LICENSE`).
