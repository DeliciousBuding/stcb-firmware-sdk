# STC-B Device Protocol v1

最后更新：2026-09-09

STC-B Device Protocol v1 是 STC-B 固件与 CloudPath Driver Plugin 之间的稳定 UART 契约。它只描述设备事实与控制，不包含药盒业务。

## 1. 传输与边界

- UART：115200 baud、8N1。
- 每条消息是一行 UTF-8/ASCII，使用 CRLF 结束；接收端也接受单独 CR 或 LF。
- 固件命令行最大 70 字节（不含行尾）；命令 ID 最大 11 字节。
- 一行只能承载一条消息；未知字段必须忽略，以支持同一 major 版本内追加字段。
- Production Driver 必须逐字节节流发送（当前板级安全值 5ms/byte）；STC-B BSP UART RX 缓冲只有 1 字节，整帧 burst write 会丢字节。

## 2. 启动与版本

固件启动时发送：

~~~text
HELLO:stcb-full:v1.3.0:proto=1:baud=115200
CAPS:clock,date,temperature,illuminance,nav,ext0,ext1,hall,vibration,key1,key2,key3,buzzer,led,display,display-pages,motor,rtc-sync,diag
~~~

- HELLO 是固件身份、协议 major 和串口参数的权威声明。
- CAPS 是该固件构建实际提供的能力；Driver 不得凭 manifest 虚构硬件能力。

## 3. 状态

固件每秒主动发送一条完整状态：

~~~text
STATE:seq=00AF,clock=17:20:03,temp=1D6,light=038,nav=3FF,ext0=000,ext1=001,hall=00,vib=00,k1=00,k2=00,k3=00,navkey=00,motor=free,beep=free,led=00,display=clock,page=clock
~~~

- seq：16-bit 大写十六进制序列，回绕允许；用于诊断丢帧，不替代 CloudPath DriverMessage sequence。
- clock：HH:MM:SS。
- temp/light/nav/ext0/ext1：3 位十六进制原始 ADC（0x000–0x3FF）。温度单位换算由 Driver 完成。
- hall/vib/k1/k2/k3：00 或 01。Hall 直接读取 P1.2 电平，01 表示磁场存在。
- navkey：00=空闲、01=右、02=下、03=中心、04=左、05=上、06=K3。
- led：L0-L7 的 8-bit 十六进制 mask；display：clock（板端自动页）或 manual（主机手动 digits/codes）。
- page：自动页当前值，`clock`、`date`、`sensors`、`io` 或 `version`。v1.3.0 起追加；旧版 v1.2 设备不发送该字段，Driver 必须兼容缺省。
- motor/beep：free 或 busy。

## 4. 事件

~~~text
EVENT:hall=close
EVENT:hall=away
EVENT:vib=quake
EVENT:key1=press
EVENT:key1=release
EVENT:nav=3:press
EVENT:nav=3:release
EVENT:key3:press
EVENT:key3:release
~~~

事件是边沿事实；STATE 是当前电平事实。Driver 必须分别上报 Event 与 Observation，不能用事件缓存冒充当前状态。

## 5. 命令、ACK 与 ERROR

命令格式：

~~~text
CMD:<id>:<verb>[:key=value[,key=value...]]
~~~

成功：ACK:<id>:ok。失败：ERR:<id>:badarg、ERR:<id>:busy 或 ERR:<id>:unknown。

- UART write success 只表示主机把字节交给串口，绝不算设备成功。
- Driver 必须等待相同 id 的 ACK/ERR；超时按失败处理。
- LED/Display 在板端 API 已执行后 ACK。
- Buzzer 在实际发声完成后 ACK。
- Motor 在实际转动完成后 ACK；motorstop 在停止调用完成后 ACK。
- 同一板上的命令串行执行，因为固件仅保留一个 pending ACK；不同板可并发。

## 6. v1 verbs

| Verb | Args | 结果 |
|---|---|---|
| hello | 无 | 重发 HELLO + CAPS |
| state | 无 | 请求一次 STATE |
| diag | 无 | 返回 DIAG + ACK |
| sync | time=HHMMSS | 设置板端软件时钟；后续 STATE 提供结果读回 |
| beep | freq=<Hz>,dur=<10ms units> | 蜂鸣完成后 ACK |
| song | name=little-star\|birthday\|ode-to-joy | 固件原生音序器连续播放；整首完成后 ACK |
| led | mask=<00..FF> | LED 位掩码生效后 ACK |
| display | digits=<8 chars> | 手动显示；字符支持 0-9/- 及 H/L/S/T/C/B/K/V/P/E/R/A/U/O/N |
| display | mode=clock | 恢复板端 HH-MM-SS 每秒显示 |
| display | mode=date | 显示 DS1302 日期 YYYYMMDD |
| display | mode=sensors | 显示 `TxxxLxxx` 温度/光敏原始 ADC（3 位十六进制） |
| display | mode=io | 显示 `HxVxK123` 霍尔、振动、K1/K2/K3 电平 |
| display | mode=version | 显示 `StCb130-`（STC-B v1.3.0） |
| motor | speed=<1..255>,steps=<nonzero> | 转动完成后 ACK |
| motorstop | 无 | 紧急停止 |

`song` 只接受内置曲目 ID，旋律表放 `code` Flash，播放状态放 `xdata`；10ms 拍推进音符，播放期间暂停 UART 事件/STATE 发送以避免与 CCP 蜂鸣冲突。

上电默认数码管为 HH-MM-SS 实时时钟；手动 `display digits=...` / `codes=...` 会切换到 manual 模式。K1 短按在五个自动页之间切换，15 秒无操作回到 clock；本地翻页仍发送 `EVENT:key1=press`，不吞并按键事件。主机 `display mode=...` 选页后保持在该页，直到下一条显示命令或本机 K1 操作。

## 7. TIME_SYNC

- Driver 在设备 Watch 建立后立即发送一次 sync，以后每 10 分钟校时；命令包含秒并补偿串口传输时间。
- 固件启动时读取 DS1302 作为种子，运行中使用单一软件 HH:MM:SS 状态源，避免损坏的 hour 寄存器和偏移叠加。
- 同步的最终证据是后续 STATE 中的 clock，不能只看串口写入。

## 8. DIAGNOSTICS

~~~text
DIAG:p1=FF,p2=FF,p3=FF,hall_pin=0,vib_pin=0
~~~

DIAG 用于定位端口电平、焊接和引脚问题，不进入普通 Capability 状态。诊断内容不得包含凭据或主机路径。

## 9. 兼容策略

- proto=1 的字段可追加，既有字段与含义不可原地改变。
- 破坏性变更发布 proto=2；Driver 可并行支持多个 major，但不得猜测。
- 固件暂时接受 V/B/L/N/T legacy 帧用于 bring-up；legacy ACK 使用 id=0，不具备生产级关联语义。CloudPath 正式闭环只使用 `CMD:<id>:...`。
- legacy 单字符 `D` 表示**进入 ISP 下载模式**（12 秒倒计时后 `IAP_CONTR=0xE0` 软复位，v1.2 起；v1.0 为 5 秒——stcgal 冷启动可能超过 5s 导致竞争失败），与 `tools/stcflash.py` 的全自动烧录约定一致；诊断只通过 `CMD:<id>:diag` 触发，任何上位机都不得把诊断命令编码成 `D`。
- legacy `D` 必须以固件实际波特率发送（Full Firmware v1.2/v1.3.0 = 115200），并逐字节节流为 `D\r\n`；`tools/stcflash.py` 会等待 `ACK:0:ok` 作为固件接受 D 的证据。波特率不匹配时字节被当作噪声丢弃，自动烧录会降级为手动。
