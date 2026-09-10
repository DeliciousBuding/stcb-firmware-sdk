/* SPDX-License-Identifier: Apache-2.0 */

// STC-B 传感器+执行器探针固件（sensor-probe）
// 用途：作为 cloudpath stcb 适配器 v2 线协议的**真实载体**（V/B/L/N/T）。
// 无药盒调度逻辑，独立目录（参照 diag-rtc 模式），绕开 restricted-Keil 2KB 限制。
//
// 取舍（2KB 预算内，按 Captain 优先级）：
//   V(全量传感器+秒) 与 B(蜂鸣) 硬性；L(LED)/N(数码管)/T(对时) 保留；
//   M(步进电机) 与 S(简化转储) 因 2KB 未包含（电机驱动库较大）；如预算放宽可加回。
//
// 板级注记（沿用 demo 固件结论）：
//   * DS1302 hour 本板坏（回写 0x92），hour 由软件维护（分钟翻转 +1）；min/sec 寄存器可靠。
//   * 同拍 SetBeep + UART TX 互相损坏（CCP 中断风暴）：本探针所有串口回复集中在
//     1S 拍；蜂鸣由 B 命令缓存到下一拍再触发，且 UART 回复只在 BeepFree 时发，规避同拍/响铃期 UART 损坏。
//   * 传感器一律回原始 ADC/电平，不做单位换算（温度/光强换算归适配器/展示层）。
//   * hall/vib/k1 用 BSP 事件锁存（GetHallAct/GetVibAct/GetKeyAct）报「待处理触发事件=1否则0」，不伪造。
//
// 命令（ASCII 帧，CRLF 结尾）：
//   V        -> V:<hh><mm><ss><st><rt><rop><nav><ext0><ext1><hall><vib><k1>
//   B+dd     -> 蜂鸣（d=频率档 0-9，d=时长档 0-9）
//   L+d      -> LED（0=灭，1-8=对应LED，9=全亮 0xFF）
//   N+8digits-> 数码管（8 个 decode_table 编号 0-9=数字）
//   T+HHMM   -> 对时（软件 hour 直设 + min/sec 偏移，目标秒=00 相位归整）
//   D        -> 5s 后软复位进 ISP（stcflash 全自动烧录约定）
//   S        -> 兼容 legacy 轮询：等价请求一次 V 帧
//   M/S/O/R: 未包含（见上）。
//
// 内存策略：大缓冲放 xdata（外部 RAM 充足），避免 8051 直接 data 128B 溢出。

#include "STC15F2K60S2.H"
#include "sys.H"
#include "displayer.H"
#include "DS1302.h"
#include "hall.H"
#include "Beep.h"
#include "uart1.h"
#include "Key.H"
#include "adc.h"
#include "Vib.h"

code unsigned long SysClock = 11059200;

code char TAG_BOOT[] = "SENSOR-PROBE";

#ifdef _displayer_H_
code char decode_table[] = {
    0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f,
    0x00,0x08,0x40,0x01,0x76,0x38,
    0x3f|0x80,0x06|0x80,0x5b|0x80,0x4f|0x80,0x66|0x80,
    0x6d|0x80,0x7d|0x80,0x07|0x80,0x7f|0x80,0x6f|0x80
};
#endif

/* ---------------- 软件时间（hour 软件维护；min/sec 来自 DS1302 可靠寄存器） ---------------- */
static unsigned char sw_hour = 0x08;
static unsigned char last_min = 0xFF;
/* T 对时偏移（Plan B：零芯片写） */
static unsigned char sync_n = 0;
static xdata unsigned char sync_buf[4];
static unsigned char sync_ready = 0;
static unsigned char sw_min_off = 0;
static unsigned char sw_sec_off = 0;

/* ---------------- 串口命令通道（单字节缓冲 + B/L/N 多字节载荷状态机） ---------------- */
char rxbuf;                             /* SetUart1Rxd 接收缓冲 */
static unsigned char pending_cmd = 0;   /* 单字节命令：V */
static unsigned char rx_cmd = 0;        /* 多字节命令字 B/L/N */
static unsigned char rx_n = 0;
static unsigned char rx_need = 0;
static xdata unsigned char rx_payload[8];
static unsigned char rx_ready = 0;      /* 载荷收齐，1S 拍执行 */

/* ---------------- 回复/蜂鸣调度（规避同拍/响铃期 UART 损坏） ---------------- */
static unsigned char reply_v = 0;       /* 1=待发 V 帧 */
static unsigned char reply_mm = 0, reply_ss = 0;
static unsigned char beep_pending = 0;
static unsigned int  beep_freq = 0;
static unsigned int  beep_dur = 0;
static unsigned char isp_countdown = 0;

/* 复用缓冲：放 xdata，释放直接 data 空间（否则 8051 data 128B 溢出） */
static xdata char vbuf[32];
static xdata char lbuf[24];
static xdata unsigned char segbuf[8];

#define HC(n) (char)('0' + (((unsigned char)(n)) & 0x0F))   /* BCD 半字节直出字符 */

/* 十六进制半字节出字符（ADC 0x000-0x3FF 用） */
static char hexn(unsigned char v)
{
    v &= 0x0F;
    return (v < 10) ? (char)('0' + v) : (char)('A' + v - 10);
}

/* BCD 加 1，超过 maxv 回 0（hour 翻转） */
static unsigned char bcd_inc(unsigned char v, unsigned char maxv)
{
    v++;
    if ((v & 0x0F) > 0x09) v += 0x06;
    if (v > maxv) v = 0x00;
    return v;
}

/* BCD + BCD 偏移 mod 60 -> BCD（T 对时每拍校正 min/sec） */
static unsigned char bcd_add_off(unsigned char v, unsigned char off)
{
    unsigned char r;
    if (off == 0) return v;
    if ((unsigned char)((v & 0x0F) + (off & 0x0F)) > 0x09)
        r = (unsigned char)(v + off + 0x06);
    else
        r = (unsigned char)(v + off);
    if ((r >> 4) >= 6) r = (unsigned char)(r - 0x60);
    return r;
}

/* ASCII 两位 -> BCD mod60 差值（b - a） */
static unsigned char bcd_diff60(unsigned char a_bcd, unsigned char b_bin)
{
    unsigned char a, d, tens = 0;
    a = (unsigned char)((a_bcd >> 4) * 10 + (a_bcd & 0x0F));
    d = (unsigned char)(b_bin + 60 - a);
    if (d >= 60) d = (unsigned char)(d - 60);
    while (d >= 10) { d = (unsigned char)(d - 10); tens++; }
    return (unsigned char)((tens << 4) | d);
}

/* 芯片 hour -> 有效 24h BCD（bit7 置位=本板垃圾 0x92，回退 0x08） */
static unsigned char hour_from_chip(unsigned char chiph)
{
    if (chiph & 0x80) return 0x08;
    return (chiph > 0x23) ? 0x08 : chiph;
}

/* T 对时 Plan B：软件偏移，零芯片写 */
static void apply_sync(unsigned char cmm, unsigned char css)
{
    sw_hour = (unsigned char)(((sync_buf[0] - '0') << 4) | (sync_buf[1] - '0'));
    sw_min_off = bcd_diff60(cmm, (unsigned char)((sync_buf[2] - '0') * 10 + (sync_buf[3] - '0')));
    sw_sec_off = bcd_diff60(css, 0);   /* 目标秒=00：相位归整到整秒 */
    last_min = 0xFF;
}

/* 蜂鸣频率档位表 (Hz) / 时长档位表 (10ms)——契约§3 裁剪映射，帧宽序不变 */
code unsigned int  freq_tbl[10] = {0,500,800,1000,1200,1500,2000,2500,3000,0};
code unsigned char dur_tbl[10]  = {5,10,15,18,25,40,60,90,120,0};

/* 非阻塞发送（等空闲再发，避免叠发丢字符） */
static void send_bytes(char *b, unsigned char n)
{
    while (GetUart1TxStatus() != enumUart1TxFree);
    Uart1Print(b, n);
}

static void send_line(char code *s)
{
    unsigned char i = 0;
    while (s[i] != 0 && i < 20) { lbuf[i] = s[i]; i++; }
    lbuf[i++] = '\r'; lbuf[i++] = '\n';
    send_bytes(lbuf, i);
}

/* V 帧全量传感器快照 */
static void send_v_frame(unsigned char mm, unsigned char ss)
{
    unsigned char i = 0;
    struct_ADC a;
    unsigned char hv, vv, kv;

    a = GetADC();
    hv = (GetHallAct() != enumHallNull) ? 1 : 0;
    vv = (GetVibAct() != enumVibNull) ? 1 : 0;
    kv = (GetKeyAct(enumKey1) == enumKeyPress) ? 1 : 0;

    vbuf[i++] = 'V'; vbuf[i++] = ':';
    vbuf[i++] = HC(sw_hour >> 4); vbuf[i++] = HC(sw_hour);
    vbuf[i++] = HC(mm >> 4);      vbuf[i++] = HC(mm);
    vbuf[i++] = HC(ss >> 4);      vbuf[i++] = HC(ss);
    vbuf[i++] = '0';                          /* 本探针无药盒状态，恒 0 */
    vbuf[i++] = hexn(a.Rt >> 8);      vbuf[i++] = hexn(a.Rt >> 4);      vbuf[i++] = hexn(a.Rt);
    vbuf[i++] = hexn(a.Rop >> 8);     vbuf[i++] = hexn(a.Rop >> 4);     vbuf[i++] = hexn(a.Rop);
    vbuf[i++] = hexn(a.Nav >> 8);     vbuf[i++] = hexn(a.Nav >> 4);     vbuf[i++] = hexn(a.Nav);
    vbuf[i++] = hexn(a.EXT_P10 >> 8); vbuf[i++] = hexn(a.EXT_P10 >> 4); vbuf[i++] = hexn(a.EXT_P10);
    vbuf[i++] = hexn(a.EXT_P11 >> 8); vbuf[i++] = hexn(a.EXT_P11 >> 4); vbuf[i++] = hexn(a.EXT_P11);
    vbuf[i++] = hexn(hv); vbuf[i++] = hexn(vv); vbuf[i++] = hexn(kv);
    vbuf[i++] = '\r'; vbuf[i++] = '\n';
    send_bytes(vbuf, i);
}

/* 执行器执行（无药盒状态）：L/N 立即；B 缓存到下一拍（蜂鸣）避免同拍 UART 损坏 */
static void exec_actuator(unsigned char cmd, unsigned char p[])
{
    unsigned char i, d;
    if (cmd == 'B') {
        d = (unsigned char)(p[0] - '0'); if (d > 9) d = 0;
        i = (unsigned char)(p[1] - '0'); if (i > 9) i = 0;
        beep_freq = freq_tbl[d];
        beep_dur  = dur_tbl[i];
        beep_pending = 1;                 /* 缓拍触发，规避同拍 UART+Beep */
    } else if (cmd == 'L') {
        d = (unsigned char)(p[0] - '0'); if (d > 9) d = 0;
        if (d == 0)      LedPrint(0x00);
        else if (d == 9) LedPrint(0xFF);
        else             LedPrint((unsigned char)(1 << (d - 1)));
    } else if (cmd == 'N') {
        for (i = 0; i < 8; i++) {
            d = (unsigned char)(p[i] - '0');
            segbuf[i] = (d > 9) ? 10 : d;
        }
        Seg7Print(segbuf[0], segbuf[1], segbuf[2], segbuf[3], segbuf[4], segbuf[5], segbuf[6], segbuf[7]);
    }
}

/* UART RXD 回调：字节级状态机（T 对时 + B/L/N 载荷 + V） */
void cbrx(void)
{
    unsigned char c = (unsigned char)rxbuf;
    if (c == 'D') { isp_countdown = 5; return; }
    if (c == 'S') { pending_cmd = 'V'; return; }   /* legacy 驱动 S+V 快发兼容 */
    if (c == 'T') { sync_n = 0; rx_cmd = 0; rx_n = 0; return; }
    if (c == '\r' || c == '\n') return;   /* 行尾不能覆盖 pending_cmd（V 可带/不带 CRLF） */
    if (rx_cmd != 0) {
        if (c >= '0' && c <= '9') {
            rx_payload[rx_n++] = c;
            if (rx_n >= rx_need) rx_ready = 1;
            return;
        }
        rx_cmd = 0; rx_n = 0;
    }
    if (c >= '0' && c <= '9' && sync_n < 4) {
        sync_buf[sync_n++] = c;
        if (sync_n == 4) sync_ready = 1;
        return;
    }
    sync_n = 0;
    if (c == 'B' || c == 'L' || c == 'N') {
        rx_cmd = c; rx_n = 0;
        if (c == 'N') rx_need = 8;
        else rx_need = 2;
        return;
    }
    pending_cmd = c;   /* 单字节命令：V（其余忽略） */
}

/* 1S 拍：读时 + 对时 + 命令调度（UART 回复只在 BeepFree；蜂鸣缓到无回复拍） */
void cb1s(void)
{
    struct_DS1302_RTC t;
    unsigned char mm, ss;

    t = RTC_Read();
    mm = t.minute; ss = t.second;
    if (sync_ready) { sync_ready = 0; apply_sync(mm, ss); }
    mm = bcd_add_off(mm, sw_min_off);
    ss = bcd_add_off(ss, sw_sec_off);
    if (last_min != 0xFF && mm < last_min) sw_hour = bcd_inc(sw_hour, 0x23);
    last_min = mm;

    /* 执行器：L/N 立即；B 置 beep_pending */
    if (rx_ready) {
        exec_actuator(rx_cmd, rx_payload);
        rx_ready = 0; rx_cmd = 0; rx_n = 0;
    }
    if (pending_cmd == 'V') {
        reply_v = 1;
        reply_mm = mm; reply_ss = ss;
    }
    pending_cmd = 0;

    /* 蜂鸣：仅当本拍无 UART 回复且 UART 空闲时触发（缓拍），规避同拍损坏 */
    if (beep_pending && reply_v == 0 && GetUart1TxStatus() == enumUart1TxFree) {
        SetBeep(beep_freq, beep_dur);
        beep_pending = 0;
    }
    /* UART 回复：仅在 BeepFree（蜂鸣未响）时发，规避响铃期损坏 */
    if (reply_v && GetBeepStatus() == enumBeepFree) {
        send_v_frame(reply_mm, reply_ss);
        reply_v = 0;
    }

    /* D：延迟软复位进 ISP。保留该命令是烧录红线——Edge 常跑此固件时，
       不能要求每次烧录都人工按 Reset。 */
    if (isp_countdown != 0) {
        isp_countdown--;
        if (isp_countdown == 0) {
            IAP_CONTR = 0xE0;
            while (1);
        }
    }
}

void main(void)
{
    struct_DS1302_RTC t, init_time;

    DisplayerInit();
    SetDisplayerArea(0, 7);
    Seg7Print(10, 10, 10, 10, 10, 10, 10, 10);

    BeepInit();
    HallInit();
    Uart1Init(115200);
    KeyInit();
    AdcInit(ADCincEXT);
    VibInit();

    /* 掉电（RTC 数据失效）时以该时间初始化（写 DS1302 仅此一次，启动后只读） */
    init_time.second = 0x00;
    init_time.minute = 0x00;
    init_time.hour   = 0x08;
    init_time.day    = 0x02;
    init_time.month  = 0x09;
    init_time.week   = 0x03;
    init_time.year   = 0x26;
    DS1302Init(init_time);

    t = RTC_Read();
    sw_hour = hour_from_chip(t.hour);
    last_min = t.minute;

    SetUart1Rxd(&rxbuf, 1, 0, 0);
    SetEventCallBack(enumEventSys1S, cb1s);
    SetEventCallBack(enumEventUart1Rxd, cbrx);

    MySTC_Init();
    send_line(TAG_BOOT);
    while (1) { MySTC_OS(); }
}
