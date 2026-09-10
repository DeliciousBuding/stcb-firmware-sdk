# iap-probe — IAP 自编程探针

最后更新：2026-09-10

## 它回答什么问题

**IAP15F2K61S2 能不能在「用户程序区」擦 / 写 / 读自己的 Flash？**

这一个问题是「设备自写固件更新（真 OTA）」的总开关：通不了，后面所有传输协议、双槽回滚、
edge 侧执行器都不必设计；通了，才谈得上把「远程触发 stcgal 抢 ISP 窗口」升级成设备自持的更新。

## 手册依据

`docs/3_STC15F2K60S2数据手册.pdf` 第 730 页 EEPROM 选型表：

| 型号 | EEPROM 字节数 | 扇区数 | IAP 字节读地址范围 | 备注 |
|---|---|---|---|---|
| STC15F2K60S2 | 1K | 2 | 0000h..03FFh | 有专门 EEPROM 区 |
| **IAP15F2K61S2** | **-** | **122** | **0000h..F3FFh** | **「没有专门的 EEPROM，但可在用户程序区修改用户程序，使用时不要将自己的有效程序擦除」** |

同手册 9.1 节寄存器：

- `IAP_CMD` `MS1:MS0` = `00` 待机 / `01` 字节读 / `10` 字节编程 / `11` 扇区擦除；**扇区 512 字节**
- `IAP_TRIG` 先写 `5Ah` 再写 `A5h` 才生效；每次触发前需重送命令
- `IAP_CONTR` bit7 `IAPEN=1` 使能；bit4 `CMD_FAIL=1` 表示地址非法，需软件写 0 清零
- `IAP_CONTR` `WT2:WT1:WT0` 等待时间：`011` = 推荐系统时钟 ≤12MHz（本板 11.0592MHz）→ `IAP_CONTR = 0x83`

## 命令

| 命令 | 作用 |
|---|---|
| `I` | 跑一次自测并逐条上报 `IAP:RESULT:step=... state=...` |
| `S` | 重发上次结果（不重跑） |
| `D` | 延迟 12s 软复位进 ISP —— **烧录链红线，常驻固件必须保留 D** |

自测序列（六步，先制造证据再销毁证据）：

1. `read_pre` — 全扇区扫描基线，应 `blank`
2. `program` — 字节编程扇区首 16 字节 + **末字节**（覆盖扇区首尾，避免只测第一个字节的假阳性）
3. `read_post` — 读回比对，应 `match`
4. `erase` — 扇区擦除
5. `read_after_erase` — 全扇区复扫应重新 `blank`（**擦除只有在写入过之后才可证伪**）
6. `cmdfail` — 对越界地址 `F400h` 发起读，应 `raised`（越界不会被静默接受）

PASS 判据只取 1–5；`cmdfail` 是安全属性，单独上报，不计入成败。

## 用法

```powershell
python build.py              # 编译（含代码体积门禁）
python build.py --flash      # 编译 + 全自动烧录（板上当前固件带 D 时零按键）
python run_test.py           # 跑一次自测并断言
python run_test.py --dump    # 只重发上次结果
python run_test.py --evidence evidence-20260910.txt
```

## 结果（2026-09-10 真板）

```
IAP:START:addr=E000:section=112:512bytes
IAP:RESULT:step=read_pre state=blank nonff=0
IAP:RESULT:step=program state=ok
IAP:RESULT:step=read_post state=match data=5AA500FF112233445566778899AABBCC
IAP:RESULT:step=erase state=ok
IAP:RESULT:step=read_after_erase state=blank nonff=0
IAP:RESULT:step=cmdfail state=raised addr=F400
IAP:RESULT:step=total state=PASS
```

原始证据：`evidence-20260910.txt`。**三次 PASS**（13:19:10 / 13:19:23 / 13:26:41），
同一块板、同一扇区。烧录 18s 零按键（`D` 机制）。

同一份证据里还有一条**对照跑**（13:25:38）：板上是 `stcb-full`，runner 如实报
`[FAIL] 未收到 IAP:RESULT:step=total`，并原样记录了 stcb-full 的 `STATE:` 帧。
保留它是为了说明这个 runner 不做宽松判定——换了固件就报 FAIL，不会把别的东西当 PASS。

完整往返也实测过：`stcb-full` → 烧探针(18s) → 自测 PASS → 烧回 `stcb-full`(20s)，
三次烧录全程零按键——探针自身也保留 `D`，所以「临时闪入再还原」不需要人工按 Reset。

**结论：整块 61 KiB（122 扇区 × 512B）都可由固件自己擦写，且越界地址会被 `CMD_FAIL` 拒绝。
设备自写固件更新（真 OTA）的地基成立。**

## 安全约束（改这个探针前必读）

1. **测试扇区** `IAP_TEST_ADDR = 0xE000`（第 112/122 扇区），远高于探针有效程序。
   `build.py` 会解析链接输出的 `code=`，一旦逼近 `0xE000` 直接编译失败——
   手册原话是「不要将自己的有效程序擦除」，这条门禁就是它的机器化。
2. **不发声**：探针不调用 `SetBeep`。板子上「同拍蜂鸣 + UART TX 互相损坏」的雷因此不存在。
3. **IAP 期间关中断**（手册 16.7）。单次扇区擦除约 21ms，会打断 BSP 的 1ms 调度，
   表现为 `PollingMisses` 上升——本探针是 bring-up 工具，不承担实时业务。
4. **测试是自清理的**：跑完最后一件事是擦除，扇区被留回空白，不污染板子。

## 踩到的板级坑（已修，勿回退）

**`Uart1Print` 直接引用传入缓冲，不拷贝。** 若上一行仍在发送时就改写该缓冲，
上一行的尾部会被本行内容替换，实测表现为两行交错粘连，例如：

```
IAP:RESULT:step=read_post state=maIAP:RESULT:step=erase state=ok
match data=5AA500FF112233445566778899AABBCC
```

所以 `out_reset()` 必须**先等发送空闲再动缓冲**，`out_flush()` 必须重试到 `enumUart1TxOK`
（`Uart1Print` 非阻塞，串口忙时返回失败且不发送）。这是本探针唯一的串口纪律。

## 下一步（仍未做，属候选）

- 双槽 + 版本回滚：需要先定 app slot 划分与「起不来就跑引导」的判定。
- 传输协议：分块 + 校验 + 重传，避开 stcgal 那种连续流。
- 这仍是 `IDEA` 级：探针只证明了**能力存在**，不等于更新链路已设计或验证。
