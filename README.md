# stcb-firmware-sdk

[![Python](https://img.shields.io/badge/Python-3.12-3776AB?logo=python&logoColor=white)](https://www.python.org/)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![CI](https://github.com/DeliciousBuding/stcb-firmware-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/DeliciousBuding/stcb-firmware-sdk/actions)
[![Platform](https://img.shields.io/badge/platform-Windows%20%2F%20Linux-informational)]()


STC-B 学习板（IAP15F2K61S2）的**板级固件/硬件抽象 SDK**。供设备固件、课程工程与
`cloud-path-driver-stcb`（平台设备驱动）共享引用，是可复用、可开源的下层。

## 目录

| 目录 | 作用 |
|------|------|
| `BSP/` | 板级驱动库（`inc/*.h` 接口 + `STCBSP_V3.6.LIB` Keil C51 静态库） |
| `tools/` | 设备侧工具链：`serlink.py`(串口基建/STC_PORT/NUL清洗) · `stcflash.py`(烧录SSOT) · `serial-console.py` · `serial-log.py` |
| `examples/sensor-probe/` | 参考探针固件（全量传感器+执行器，2KB 内，验证 BSP 集成），命令集 V/B/L/N/T |


## 生态定位

与 [CloudPath](https://github.com/DeliciousBuding/cloud-path)（云原生、插件驱动的互联物联网控制平台）
配套：设备固件（本 SDK）→ 平台设备驱动（`cloud-path-driver-stcb`）→ 控制平台，构成
「板卡 → SDK → 云」的边云协同链路；本 SDK 提供板级 BSP/工具链，是可复用、可开源的下层统一底座。

## 使用

- 依赖：Keil C51（`C51/BL51/OH51`）编译，串口默认 `COM3`（env `STC_PORT` 覆盖）。
- 编译探针：`cd examples/sensor-probe && python build.py`（`--flash` 委托 `tools/stcflash.py`）。
- 注意：`build.py` 中 `KEIL_BIN`/`STC_INC` 为**本机路径占位**，请按你机器上的 Keil 安装路径调整。

## 开源范围

- 已含：**接口头文件 + BSP 静态库 + 工具链 + 参考固件**（接口级开源，可直接链接与二次开发）。
- 若需源码级开源，需补 BSP 的 `.c` 源（当前仓库未含）。
- License: MIT（见 `LICENSE`）。
