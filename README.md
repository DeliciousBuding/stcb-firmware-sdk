# stcb-firmware-sdk

[![Python](https://img.shields.io/badge/Python-3.12-3776AB?logo=python&logoColor=white)](https://www.python.org/)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![CI](https://github.com/DeliciousBuding/stcb-firmware-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/DeliciousBuding/stcb-firmware-sdk/actions)
[![Platform](https://img.shields.io/badge/firmware-Windows%20%2B%20Keil%20C51-informational)]()


STC-B 学习板（IAP15F2K61S2）的**板级固件/硬件抽象 SDK**。供设备固件、课程工程与
`cloud-path-driver-stcb`（平台设备驱动）共享引用，是可复用、可开源的下层。

## 目录

| 目录 | 作用 |
|------|------|
| `BSP/` | 板级驱动库（`inc/*.h` 接口 + `STCBSP_V3.6.LIB` Keil C51 静态库） |
| `tools/` | 设备侧工具链：`serlink.py`(串口基建/STC_PORT/NUL清洗) · `stcflash.py`(烧录SSOT) · `serial-console.py` · `serial-log.py` |
| `examples/sensor-probe/` | 2KB legacy 探针（V/B/L/N/T），只用于兼容/bring-up |
| `examples/rtc-battery-probe/` | DS1302 纽扣电池消融探针（记录本板不能依赖电池保持的实验证据） |
| `examples/iap-probe/` | IAP 自编程探针：单扇区真板证实用户程序区可擦/写/读；OTA 本身尚未实现 |
| `examples/stcb-full/` | CloudPath reference firmware v1.3.1：完整板载能力 + 五页数码管 + 原生歌曲音序器 + Device Protocol v1 + 真实 ACK/ERROR |


## 生态定位

与 [CloudPath](https://github.com/DeliciousBuding/cloud-path)（云原生、插件驱动的互联物联网控制平台）
配套：设备固件（本 SDK）→ 平台设备驱动（`cloud-path-driver-stcb`）→ 控制平台，构成
「板卡 → SDK → 云」的边云协同链路；本 SDK 提供板级 BSP/工具链，是可复用、可开源的下层统一底座。

## 使用

- 固件构建/烧录当前面向 Windows + Keil C51（`C51/BL51/OH51`）；Python 串口工具本身可跨平台运行。
- `KEIL_HOME` 覆盖默认安装目录 `C:\Keil_v5`；串口默认 `COM3`，可用 env `STC_PORT` 覆盖。
- Python 依赖：`python -m pip install pyserial stcgal`。
- 编译完整固件：`cd examples/stcb-full && python build.py`（`--flash` 委托 `tools/stcflash.py`；无法软复位时需要按板载 Reset）。
- 所有 `build.py` 都通过 `KEIL_HOME` 查找工具链，不需要修改源码中的机器路径。

## 开源范围

- 已含：**接口头文件 + BSP 静态库 + 工具链 + 参考固件**（接口级开源，可直接链接与二次开发）。
- 若需源码级开源，需补 BSP 的 `.c` 源（当前仓库未含）。
- License: MIT（见 `LICENSE`）。
