# STC-B Full Firmware v1.2

最后更新：2026-09-09

CloudPath 的 STC-B reference firmware。它是板载硬件固件，不包含药盒业务；线协议见 [STC-B Device Protocol v1](../../docs/stcb-device-protocol-v1.md)。

## 板载能力

- RTC / HH-MM-SS 数码管实时时钟
- Temperature、Illuminance、KN navigation ADC、EXT0/EXT1 ADC
- Hall 当前电平 + close/away 事件
- Vibration 当前电平 + quake 事件
- K1/K2/K3 当前状态 + press/release 事件
- Buzzer、原生歌曲音序器（`little-star` / `birthday` / `ode-to-joy`）、L0-L7 LED bank、8-digit display、step-motor connector
- Version/Capabilities、STATE、EVENT、COMMAND、ACK/ERROR、TIME_SYNC、DIAGNOSTICS

L0-L7 通过 8-bit mask 独立控制；数码管支持时钟模式、8 位数字和板载 decode table 的 0-25 字形码。

## 构建和烧录

~~~powershell
$env:KEIL_HOME = 'C:\Keil_v5' # 可省略
$env:STC_PORT = 'COM3'         # 可省略
python build.py
python build.py --flash
~~~

`--flash` 走仓库 `tools/stcflash.py`：逐字节节流发送 legacy `D\r\n`，等待 `ACK:0:ok` 后进入
12 秒 ISP 倒计时；stcgal 握手接住即零按键全自动，接不住才降级手动。

原生歌曲由 10ms 拍推进，整首播放完成后才回 ACK；旋律表在 `code` Flash，播放状态在 `xdata`。

真实成功必须以串口 ACK/ERROR、STATE/EVENT 和物理现象为依据。没有接入步进电机时，只能证明接口、构建和命令链，不能宣称电机物理完成。
