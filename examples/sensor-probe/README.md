# STC-B 传感器+执行器探针固件（sensor-probe）

> 独立探针固件：作为 cloudpath stcb 适配器 v2 线协议的**真实载体**（V/B/L/N/T）。
> 无药盒调度逻辑，绕开 restricted-Keil 2KB 限制（药盒 demo build 因 2KB 无法承载 v2）。
> 线协议 SSOT：`cloudpath/.local/plan/v0.1-sensor-v2-contract.md`（§2 V 帧 / §3 执行器帧）。

## 用途

demo 药盒固件的 demo build 被 Keil C51 **Eval 版 0800H(2KB)** 代码上限锁死，无法塞入 ADC/Vib/StepMotor 传感器/执行器（且不能砍用户函数——板级注记 10 UART-TX 红线）。故按契约建此**独立探针**（参照 `diag-rtc/` 模式），在同块 STC-B 上实现 v2 线协议，供适配器/edge 读取与驱动。

## 命令（ASCII 帧，CRLF 结尾）

| 命令 | 语义 | 帧 |
|---|---|---|
| `V` | 全量传感器快照 | `V:<hh><mm><ss><st><rt><rop><nav><ext0><ext1><hall><vib><k1>` |
| `B`+2 | 蜂鸣（频率档/时长档） | 高=频率档 0-9，低=时长档 0-9 |
| `L`+1 | LED（0=灭,1-8=对应LED,9=全亮 0xFF） | 1×ASCII 0-9 |
| `N`+8 | 数码管（8 个 decode_table 编号 0-9=数字） | 8×ASCII 0-9 |
| `T`+4 | 对时（HHMM，目标秒=00 相位归整） | 单字节 `T` + 4×ASCII 数字 |
| `D` | 软复位进 ISP（5s 延迟） | 单字节 `D` |

| 命令 | 语义 | 状态 |
|---|---|---|
| `V` | 全量传感器快照 | ✅ |
| `B`+2 | 蜂鸣 | ✅ |
| `L`+1 | LED | ✅ |
| `N`+8 | 数码管 | ✅ |
| `T`+4 | 对时 | ✅ |
| `M`+1 | 步进电机 | ⚠️ **未包含**（2KB 预算取舍，见下） |
| `S`/`O`/`R` | 药盒转储/开盖/提醒 | ⚠️ **未包含**（本探针无药盒状态/不需要） |

## V 帧字段

```
V:<hh><mm><ss><st><rt><rop><nav><ext0><ext1><hall><vib><k1>
```
- `hh/mm/ss`：软件时间 BCD（hour 由分钟翻转 +1 软件维护，min/sec 来自 DS1302 可靠寄存器；`T` 对时后为同步时间）。
- `st`：固定 `0`（本探针无药盒状态）。
- `rt/rop/nav/ext0/ext1`：**原始 ADC**（10bit hex `0x000-0x3FF`），`GetADC().Rt/Rop/Nav/EXT_P10/EXT_P11`，不做单位换算。
- `hall/vib/k1`：0/1，BSP 事件锁存（`GetHallAct/GetVibAct/GetKeyAct`）报「待处理触发事件=1否则0」，不伪造。

## 蜂鸣/对时映射（2KB 内的裁剪表，帧宽序不变）

- 频率档：0=静音 1=500Hz 2=800Hz 3=1kHz 4=1.2kHz 5=1.5kHz 6=2kHz 7=2.5kHz 8=3kHz 9=自定义(本固件=静音)。
- 时长档：0=50ms 1=100ms 2=150ms 3=180ms 4=250ms 5=400ms 6=600ms 7=900ms 8=1.2s 9=自定义(本固件=0)。

## 2KB 取舍（按 Captain 优先级）

硬性保留：`V`(传感器+秒) + `B`(蜂鸣) + `D`(ISP 自动烧录)。尽量保留：`L`(LED)/`N`(数码管)/`T`(对时)。
放弃：`M`(步进电机——StepMotor 驱动库较大) 与 `S`(简化转储——V 帧已含全部信息)。
若预算放宽（换注册版 Keil）可加回 `M`/`S`。

## 板级注记

- DS1302 hour 本板坏（回写 0x92），hour 软件维护；min/sec 可靠。
- 同拍 `SetBeep` + UART TX 相损坏：本探针所有串口回复集中 1S 拍，`B` 蜂鸣缓拍触发，且 UART 回复只在 `BeepFree` 时发，规避 CCP 中断风暴。
- 传感器一律回原始 ADC/电平，换算归适配器/展示层。

## 编译 / 烧录

```bash
cd examples/sensor-probe
python build.py        # 编译 -> main.hex（LINK/LOCATE RUN COMPLETE，<0800H）
python build.py -f     # 烧录（仅 Captain 安全时段执行；勿打断现有 edge/plugin 链路）
```
