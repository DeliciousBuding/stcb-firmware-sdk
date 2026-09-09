# tools — 设备侧工具链

STC-B 固件开发/调试的通用工具，被各应用 `build.py` 复用。

| 工具 | 作用 |
|------|------|
| `serlink.py` | 串口基建：`STC_PORT` 端口约定 / NUL 清洗 / 端口释放 |
| `stcflash.py` | 烧录 SSOT：逐字节节流发 `D\r\n` 并等 `ACK:0:ok`，板上固件带 D 则全自动，否则按 Reset 手动 |
| `serial-console.py` | 交互控制台 |
| `serial-log.py` | 纯日志器 |

默认串口 `COM3`，`STC_PORT` 环境变量覆盖（全工具链生效）。
编译/烧录入口见各固件目录 `build.py`（`--flash` 均委托 `tools/stcflash.py`）。
