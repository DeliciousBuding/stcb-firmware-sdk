// STC-B Full Firmware v1 (stcb-full)
// CloudPath stcb 适配器的真实硬件载体。协议 v1（冻结）：见同目录 PROTOCOL.md。
//
// 引脚事实（原理图 2_STC-B学习板原理图.pdf U1 网络表坐标定案，2026-09-04）：
//   P1.0/ADC0 = EXT 扩展口 ch0      P1.1/ADC1 = EXT 扩展口 ch1
//   P1.2/ADC2 = HALL  (A3144, 10K 上拉 VUSB, 开集电极: 无磁场=1, 磁场=0)
//   P1.3/ADC3 = V_Rt  (NTC 温度分压)
//   P1.4/ADC4 = V_Ro  (光敏分压)   <- 旧速查表误标为 P1.2，已纠正
//   P1.5/ADC5 = RTC_SCLK           P1.6/ADC6 = RTC_/RST
//   P2.4      = V&P   (振动/倒置)
// 因此 霍尔 与 光敏 不共用引脚；霍尔恒 0 的根因是固件只读 BSP 边沿事件(GetHallAct)，
// 本固件改为直读 P1.2 电平（hall=磁场存在），并保留边沿事件作 EVENT 上报。
//
// 板级红线（沿用）：SetBeep 与 UART TX 不同拍；所有串口发送仅在 BeepFree 且 Tx 空闲时。
// DS1302 hour 本板坏 -> hour 软件维护；min/sec 寄存器可靠。
//
// 内存策略：大缓冲放 xdata；文件作用域静态小变量也放 xdata，压低内部 RAM。
// （BSP 库为 SMALL 模型，内部 data 上限约 120B，超标会 L105 PUBLIC REFERS TO IGNORED SEGMENT）

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
#include "StepMotor.h"

code unsigned long SysClock = 11059200;

code char TAG_BOOT[]  = "HELLO:stcb-full:v1:proto=1:baud=115200";
code char TAG_CAPS[]  = "CAPS:clock,temperature,illuminance,nav,ext0,ext1,hall,vibration,key1,key2,key3,buzzer,led,display,motor,rtc-sync,diag";

sbit HALL_PIN = P1^2;   /* 原理图定案 HALL -> P1.2 */
sbit VIB_PIN  = P2^4;   /* 原理图定案 V&P  -> P2.4 */
sbit KEY1_PIN = P3^2;   /* KEY1 低有效 */
sbit KEY2_PIN = P3^3;
sbit KEY3_PIN = P1^7;

#ifdef _displayer_H_
code char decode_table[] = {
    0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f,
    0x00,0x08,0x40,0x01,0x76,0x38,
    0x3f|0x80,0x06|0x80,0x5b|0x80,0x4f|0x80,0x66|0x80,
    0x6d|0x80,0x7d|0x80,0x07|0x80,0x7f|0x80,0x6f|0x80
};
#endif

code unsigned int freq_tbl[10] = {0,500,800,1000,1200,1500,2000,2500,3000,0};
code unsigned char dur_tbl[10]  = {5,10,15,18,25,40,60,90,120,0};

/* ---------------- 软件时间 ---------------- */
static xdata unsigned char sw_hour = 0x08;
static xdata unsigned char sw_min = 0x00;
static xdata unsigned char sw_sec = 0x00;
static xdata unsigned char sync_buf[6];

/* ---------------- 串口行缓冲 ---------------- */
char rxbuf;
static xdata char line[72];
static xdata unsigned char line_n = 0;
static xdata char line_done[72];
static xdata unsigned char line_ready = 0;

/* ---------------- 发送/执行调度 ---------------- */
static xdata unsigned char want_state = 0;
static xdata unsigned int  seq = 0;
static xdata unsigned char beep_pending = 0;
static xdata unsigned int  beep_freq = 0;
static xdata unsigned char beep_dur = 0;
static xdata unsigned char motor_ack_pending = 0;
static xdata char obuf[192];
static xdata char lbuf[24];
static xdata unsigned char segbuf[8];
static xdata unsigned char display_clock = 1;   /* 1=HH-MM-SS 实时钟，0=手动显示 */
static xdata unsigned char led_mask = 0;
static xdata unsigned char isp_countdown = 0; /* 'D' 命令：5s 后软复位进 ISP bootloader */

/* ---------------- 事件锁存（100mS 拍消费 -> EVENT 帧） ---------------- */
static xdata unsigned char ev_hall = 0;   /* 1=close 2=away */
static xdata unsigned char ev_vib  = 0;   /* 1=quake */
static xdata unsigned char ev_key  = 0;   /* 低4位=key号(1..3) 高4位=1press/2release */
static xdata unsigned char ev_nav  = 0;   /* 高4位=1press/2release，低4位=1..5方向/6=K3 */
static xdata unsigned char nav_state = 0; /* 0=无，1右2下3中4左5上6=K3 */
static xdata unsigned char key3_state = 0;

#define HC(n) (char)('0' + (((unsigned char)(n)) & 0x0F))

static char hexn(unsigned char v)
{
    v &= 0x0F;
    return (v < 10) ? (char)('0' + v) : (char)('A' + v - 10);
}
static unsigned char hexval(char c)
{
    if (c >= '0' && c <= '9') return (unsigned char)(c - '0');
    if (c >= 'A' && c <= 'F') return (unsigned char)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (unsigned char)(c - 'a' + 10);
    return 0xFF;
}

/* ---------------- BCD 工具（沿用 sensor-probe 已板测逻辑） ---------------- */
static unsigned char bcd_inc(unsigned char v, unsigned char maxv)
{
    v++;
    if ((v & 0x0F) > 0x09) v += 0x06;
    if (v > maxv) v = 0x00;
    return v;
}
static unsigned char hour_from_chip(unsigned char chiph)
{
    if (chiph & 0x80) return 0x08;
    return (chiph > 0x23) ? 0x08 : chiph;
}
static void apply_sync(void)
{
    sw_hour = (unsigned char)(((sync_buf[0] - '0') << 4) | (sync_buf[1] - '0'));
    sw_min  = (unsigned char)(((sync_buf[2] - '0') << 4) | (sync_buf[3] - '0'));
    sw_sec  = (unsigned char)(((sync_buf[4] - '0') << 4) | (sync_buf[5] - '0'));
}

/* ---------------- 串口发送（非阻塞 + 同拍红线） ---------------- */
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
static void put3hex(unsigned char *o, unsigned char *i, unsigned int v)
{
    o[(*i)++] = hexn((unsigned char)(v >> 8));
    o[(*i)++] = hexn((unsigned char)(v >> 4));
    o[(*i)++] = hexn((unsigned char)v);
}
static void putkv(char *o, unsigned char *i, char code *k, unsigned char v)
{
    unsigned char j = 0;
    o[(*i)++] = ',';
    while (k[j]) o[(*i)++] = k[j++];
    o[(*i)++] = '=';
    o[(*i)++] = HC(v >> 4); o[(*i)++] = HC(v);
}

/* STATE 帧：全量实时快照（hall 为电平语义：1=磁场存在） */
static void send_state(void)
{
    unsigned char i = 0;
    struct_ADC a;
    unsigned char hall_lvl, vib_lvl;
    char code *ms;
    a = GetADC();
    hall_lvl = (HALL_PIN == 0) ? 1 : 0;      /* A3144 开集电极：磁场=低 */
    vib_lvl  = (VIB_PIN == 0) ? 1 : 0;
    seq++;
    obuf[i++] = 'S'; obuf[i++] = 'T'; obuf[i++] = 'A'; obuf[i++] = 'T'; obuf[i++] = 'E'; obuf[i++] = ':';
    obuf[i++] = 's'; obuf[i++] = 'e'; obuf[i++] = 'q'; obuf[i++] = '=';
    obuf[i++] = hexn((unsigned char)(seq >> 12)); obuf[i++] = hexn((unsigned char)(seq >> 8));
    obuf[i++] = hexn((unsigned char)(seq >> 4));  obuf[i++] = hexn((unsigned char)seq);
    obuf[i++] = ','; obuf[i++] = 'c'; obuf[i++] = 'l'; obuf[i++] = 'o'; obuf[i++] = 'c'; obuf[i++] = 'k'; obuf[i++] = '=';
    obuf[i++] = HC(sw_hour >> 4); obuf[i++] = HC(sw_hour); obuf[i++] = ':';
    obuf[i++] = HC(sw_min >> 4);  obuf[i++] = HC(sw_min);  obuf[i++] = ':';
    obuf[i++] = HC(sw_sec >> 4);  obuf[i++] = HC(sw_sec);
    obuf[i++] = ','; obuf[i++] = 't'; obuf[i++] = 'e'; obuf[i++] = 'm'; obuf[i++] = 'p'; obuf[i++] = '='; put3hex(obuf, &i, a.Rt);
    obuf[i++] = ','; obuf[i++] = 'l'; obuf[i++] = 'i'; obuf[i++] = 'g'; obuf[i++] = 'h'; obuf[i++] = 't'; obuf[i++] = '='; put3hex(obuf, &i, a.Rop);
    obuf[i++] = ','; obuf[i++] = 'n'; obuf[i++] = 'a'; obuf[i++] = 'v'; obuf[i++] = '=';                  put3hex(obuf, &i, a.Nav);
    obuf[i++] = ','; obuf[i++] = 'e'; obuf[i++] = 'x'; obuf[i++] = 't'; obuf[i++] = '0'; obuf[i++] = '='; put3hex(obuf, &i, a.EXT_P10);
    obuf[i++] = ','; obuf[i++] = 'e'; obuf[i++] = 'x'; obuf[i++] = 't'; obuf[i++] = '1'; obuf[i++] = '='; put3hex(obuf, &i, a.EXT_P11);
    putkv(obuf, &i, "hall", hall_lvl);
    putkv(obuf, &i, "vib",  vib_lvl);
    putkv(obuf, &i, "k1", (KEY1_PIN == 0) ? 1 : 0);   /* 低有效：按下=1（电平语义，不消费边沿） */
    putkv(obuf, &i, "k2", (KEY2_PIN == 0) ? 1 : 0);
    putkv(obuf, &i, "k3", key3_state);
    putkv(obuf, &i, "navkey", nav_state);
    ms = (GetStepMotorStatus(enumStepMotor1) == enumStepMotorBusy) ? "busy" : "free";
    obuf[i++] = ','; obuf[i++] = 'm'; obuf[i++] = 'o'; obuf[i++] = 't'; obuf[i++] = 'o'; obuf[i++] = 'r'; obuf[i++] = '=';
    obuf[i++] = ms[0]; obuf[i++] = ms[1]; obuf[i++] = ms[2]; obuf[i++] = ms[3];
    ms = (GetBeepStatus() == enumBeepBusy) ? "busy" : "free";
    obuf[i++] = ','; obuf[i++] = 'b'; obuf[i++] = 'e'; obuf[i++] = 'e'; obuf[i++] = 'p'; obuf[i++] = '=';
    obuf[i++] = ms[0]; obuf[i++] = ms[1]; obuf[i++] = ms[2]; obuf[i++] = ms[3];
    obuf[i++] = ','; obuf[i++] = 'l'; obuf[i++] = 'e'; obuf[i++] = 'd'; obuf[i++] = '=';
    obuf[i++] = hexn(led_mask >> 4); obuf[i++] = hexn(led_mask);
    obuf[i++] = ','; obuf[i++] = 'd'; obuf[i++] = 'i'; obuf[i++] = 's'; obuf[i++] = 'p'; obuf[i++] = 'l'; obuf[i++] = 'a'; obuf[i++] = 'y'; obuf[i++] = '=';
    ms = display_clock ? "clock" : "manual";
    while (*ms) obuf[i++] = *ms++;
    obuf[i++] = '\r'; obuf[i++] = '\n';
    send_bytes(obuf, i);
}

/* DIAG 帧：原始端口电平（霍尔/振动引脚归属的板测证据） */
static void send_diag(void)
{
    unsigned char i = 0;
    obuf[i++] = 'D'; obuf[i++] = 'I'; obuf[i++] = 'A'; obuf[i++] = 'G'; obuf[i++] = ':';
    obuf[i++] = 'p'; obuf[i++] = '1'; obuf[i++] = '='; obuf[i++] = hexn(P1 >> 4); obuf[i++] = hexn(P1);
    obuf[i++] = ','; obuf[i++] = 'p'; obuf[i++] = '2'; obuf[i++] = '='; obuf[i++] = hexn(P2 >> 4); obuf[i++] = hexn(P2);
    obuf[i++] = ','; obuf[i++] = 'p'; obuf[i++] = '3'; obuf[i++] = '='; obuf[i++] = hexn(P3 >> 4); obuf[i++] = hexn(P3);
    obuf[i++] = ','; obuf[i++] = 'h'; obuf[i++] = 'a'; obuf[i++] = 'l'; obuf[i++] = 'l'; obuf[i++] = '_'; obuf[i++] = 'p'; obuf[i++] = 'i'; obuf[i++] = 'n'; obuf[i++] = '=';
    obuf[i++] = HC((HALL_PIN == 0) ? 1 : 0);
    obuf[i++] = ','; obuf[i++] = 'v'; obuf[i++] = 'i'; obuf[i++] = 'b'; obuf[i++] = '_'; obuf[i++] = 'p'; obuf[i++] = 'i'; obuf[i++] = 'n'; obuf[i++] = '=';
    obuf[i++] = HC((VIB_PIN == 0) ? 1 : 0);
    obuf[i++] = '\r'; obuf[i++] = '\n';
    send_bytes(obuf, i);
}

/* ---------------- 行解析工具 ---------------- */
static unsigned char findch(char *s, unsigned char n, char c, unsigned char from)
{
    unsigned char i;
    for (i = from; i < n; i++) if (s[i] == c) return i;
    return 0xFF;
}
/* [a,b) 十进制 -> uint；失败返回 0 且 *ok=0 */
static unsigned int parse_int(char *s, unsigned char a, unsigned char b, unsigned char *ok)
{
    unsigned int v = 0; unsigned char neg = 0, i;
    *ok = 1;
    if (a < b && s[a] == '-') { neg = 1; a++; }
    if (a >= b) { *ok = 0; return 0; }
    for (i = a; i < b; i++) {
        if (s[i] < '0' || s[i] > '9') { *ok = 0; return 0; }
        v = (unsigned int)(v * 10 + (unsigned char)(s[i] - '0'));
    }
    return neg ? (unsigned int)(0 - v) : v;
}
/* args 中取 k= 值的 [a,b) 区间；找不到返回 0xFF */
static unsigned char arg_span(char *s, unsigned char an, char code *k, unsigned char *va, unsigned char *vb)
{
    unsigned char i = 0, j, kl;
    kl = 0; while (k[kl]) kl++;
    while (i < an) {
        j = i;
        while (j < an && s[j] != '=' && s[j] != ',') j++;
        if (j < an && s[j] == '=' && (unsigned char)(j - i) == kl) {
            unsigned char m, same = 1;
            for (m = 0; m < kl; m++) if (s[i + m] != k[m]) same = 0;
            if (same) {
                unsigned char e = (unsigned char)(j + 1);
                while (e < an && s[e] != ',') e++;
                *va = (unsigned char)(j + 1); *vb = e;
                return 1;
            }
        }
        while (i < an && s[i] != ',') i++;
        i++;
    }
    return 0;
}

/* ---------------- 执行器 ---------------- */
static unsigned char do_beep(unsigned int freq, unsigned int dur10ms)
{
    if (freq == 0 || freq > 4000) freq = 1000;
    if (dur10ms == 0 || dur10ms > 120) dur10ms = 20;
    beep_freq = freq; beep_dur = (unsigned char)dur10ms; beep_pending = 1;
    return 0;
}
static unsigned char do_led(unsigned int mask)
{
    led_mask = (unsigned char)(mask & 0xFF);
    LedPrint(led_mask);
    return 0;
}
static void show_clock(unsigned char mm, unsigned char ss)
{
    /* d0=最左、d7=最右；12 是 '-' 段码，显示 HH-MM-SS。 */
    Seg7Print((unsigned char)(sw_hour >> 4), (unsigned char)(sw_hour & 0x0F), 12,
              (unsigned char)(mm >> 4), (unsigned char)(mm & 0x0F), 12,
              (unsigned char)(ss >> 4), (unsigned char)(ss & 0x0F));
}
static unsigned char do_display(char *s, unsigned char a, unsigned char b)
{
    unsigned char i, d;
    if ((unsigned char)(b - a) != 8) return 1;
    for (i = 0; i < 8; i++) {
        d = (unsigned char)s[a + i];
        if (d >= '0' && d <= '9') segbuf[i] = (unsigned char)(d - '0');
        else if (d == '-') segbuf[i] = 12;
        else return 1;
    }
    display_clock = 0;
    Seg7Print(segbuf[0], segbuf[1], segbuf[2], segbuf[3], segbuf[4], segbuf[5], segbuf[6], segbuf[7]);
    return 0;
}
static unsigned char do_display_codes(char *s, unsigned char a, unsigned char b)
{
    unsigned char i, hi, lo, d;
    if ((unsigned char)(b - a) != 16) return 1;
    for (i = 0; i < 8; i++) {
        hi = hexval(s[a + i * 2]); lo = hexval(s[a + i * 2 + 1]);
        if (hi == 0xFF || lo == 0xFF) return 1;
        d = (unsigned char)((hi << 4) | lo);
        if (d > 25) return 1;
        segbuf[i] = d;
    }
    display_clock = 0;
    Seg7Print(segbuf[0], segbuf[1], segbuf[2], segbuf[3], segbuf[4], segbuf[5], segbuf[6], segbuf[7]);
    return 0;
}
static unsigned char do_motor(unsigned int speed, int steps)
{
    char r;
    if (speed == 0 || speed > 255) return 1;
    if (steps == 0) return 1;
    r = SetStepMotor(enumStepMotor1, (unsigned char)speed, steps);
    if (r == enumSetStepMotorOK) motor_ack_pending = 1;
    return (r == enumSetStepMotorOK) ? 0 : 2;
}

/* ---------------- 命令执行：返回 0 ok / 1 badarg / 2 busy / 3 unknown ---------------- */
static unsigned char exec_cmd(char *verb, unsigned char vn, char *args, unsigned char an)
{
    unsigned char va, vb, ok;
    unsigned int v1, v2;
    if (vn == 5 && verb[0]=='h' && verb[1]=='e' && verb[2]=='l' && verb[3]=='l' && verb[4]=='o') {
        send_line(TAG_BOOT); send_line(TAG_CAPS); return 0;
    }
    if (vn == 5 && verb[0]=='s' && verb[1]=='t' && verb[2]=='a' && verb[3]=='t' && verb[4]=='e') { want_state = 1; return 0; }
    if (vn == 4 && verb[0]=='d' && verb[1]=='i' && verb[2]=='a' && verb[3]=='g') { send_diag(); return 0; }
    if (vn == 4 && verb[0]=='b' && verb[1]=='e' && verb[2]=='e' && verb[3]=='p') {
        v1 = 1000; v2 = 20;
        if (arg_span(args, an, "freq", &va, &vb)) v1 = parse_int(args, va, vb, &ok);
        if (arg_span(args, an, "dur",  &va, &vb)) v2 = parse_int(args, va, vb, &ok);
        return do_beep(v1, v2);
    }
    if (vn == 3 && verb[0]=='l' && verb[1]=='e' && verb[2]=='d') {
        v1 = 0;
        if (arg_span(args, an, "mask", &va, &vb)) {
            unsigned char i, h = 0;
            for (i = va; i < vb; i++) {
                char c = args[i]; h <<= 4;
                if (c >= '0' && c <= '9') h |= (unsigned char)(c - '0');
                else if (c >= 'a' && c <= 'f') h |= (unsigned char)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') h |= (unsigned char)(c - 'A' + 10);
                else return 1;
            }
            v1 = h;
        }
        return do_led(v1);
    }
    if (vn == 7 && verb[0]=='d' && verb[1]=='i' && verb[2]=='s' && verb[3]=='p' && verb[4]=='l' && verb[5]=='a' && verb[6]=='y') {
        if (arg_span(args, an, "mode", &va, &vb) && (unsigned char)(vb - va) == 5 &&
            args[va]=='c' && args[va+1]=='l' && args[va+2]=='o' && args[va+3]=='c' && args[va+4]=='k') {
            display_clock = 1; return 0;
        }
        if (arg_span(args, an, "codes", &va, &vb)) return do_display_codes(args, va, vb);
        if (!arg_span(args, an, "digits", &va, &vb)) return 1;
        return do_display(args, va, vb);
    }
    if (vn == 5 && verb[0]=='m' && verb[1]=='o' && verb[2]=='t' && verb[3]=='o' && verb[4]=='r') {
        v1 = 100; v2 = 200;
        if (arg_span(args, an, "speed", &va, &vb)) v1 = parse_int(args, va, vb, &ok);
        if (arg_span(args, an, "steps", &va, &vb)) v2 = parse_int(args, va, vb, &ok);
        return do_motor(v1, (int)v2);
    }
    if (vn == 9 && verb[0]=='m' && verb[1]=='o' && verb[2]=='t' && verb[3]=='o' && verb[4]=='r' &&
        verb[5]=='s' && verb[6]=='t' && verb[7]=='o' && verb[8]=='p') {
        EmStop(enumStepMotor1); motor_ack_pending = 0; return 0;
    }
    if (vn == 4 && verb[0]=='s' && verb[1]=='y' && verb[2]=='n' && verb[3]=='c') {
        if (arg_span(args, an, "time", &va, &vb) && (unsigned char)(vb - va) == 6) {
            sync_buf[0]=args[va]; sync_buf[1]=args[va+1]; sync_buf[2]=args[va+2]; sync_buf[3]=args[va+3];
            sync_buf[4]=args[va+4]; sync_buf[5]=args[va+5]; apply_sync(); want_state = 1; return 0;
        }
        if (arg_span(args, an, "hhmm", &va, &vb) && (unsigned char)(vb - va) == 4) {
            sync_buf[0]=args[va]; sync_buf[1]=args[va+1]; sync_buf[2]=args[va+2]; sync_buf[3]=args[va+3];
            sync_buf[4]='0'; sync_buf[5]='0'; apply_sync(); want_state = 1; return 0;
        }
        return 1;
    }
    return 3;
}

/* ---------------- ACK/ERR ---------------- */
static xdata char ack_id[12];
static unsigned char ack_code = 0xFF;   /* 0xFF=无待发 */
static void queue_ack(char *id, unsigned char n, unsigned char cd)
{
    unsigned char i;
    if (n > 11) n = 11;
    for (i = 0; i < n; i++) ack_id[i] = id[i];
    ack_id[n] = 0;
    ack_code = cd;
}
static void send_ack(void)
{
    unsigned char i = 0, j = 0;
    char code *c;
    if (ack_code == 0) { obuf[i++]='A'; obuf[i++]='C'; obuf[i++]='K'; obuf[i++]=':'; }
    else               { obuf[i++]='E'; obuf[i++]='R'; obuf[i++]='R'; obuf[i++]=':'; }
    while (ack_id[j]) obuf[i++] = ack_id[j++];
    obuf[i++] = ':';
    c = (ack_code == 0) ? "ok" : (ack_code == 1) ? "badarg" : (ack_code == 2) ? "busy" : "unknown";
    while (*c) obuf[i++] = *c++;
    obuf[i++] = '\r'; obuf[i++] = '\n';
    send_bytes(obuf, i);
    ack_code = 0xFF;
}
static void send_event(char code *body)
{
    unsigned char i = 0, j = 0;
    obuf[i++]='E'; obuf[i++]='V'; obuf[i++]='E'; obuf[i++]='N'; obuf[i++]='T'; obuf[i++]=':';
    while (body[j] && i < 150) obuf[i++] = body[j++];
    obuf[i++] = '\r'; obuf[i++] = '\n';
    send_bytes(obuf, i);
}

/* ---------------- UART RX：行缓冲 ---------------- */
void cbrx(void)
{
    unsigned char c = (unsigned char)rxbuf;
    if (c == '\r' || c == '\n') {
        if (line_n > 0) {
            unsigned char i;
            for (i = 0; i < line_n; i++) line_done[i] = line[i];
            line_done[line_n] = 0;
            line_ready = 1;
            line_n = 0;
        }
        return;
    }
    if (line_n < 70) line[line_n++] = (char)c;
}

/* ---------------- 行 -> 命令 ---------------- */
static void process_line(void)
{
    char *s = line_done;
    unsigned char n = 0, p1, p2;
    char idbuf[12];
    unsigned char idn = 0, i;
    unsigned char cd;
    while (s[n]) n++;
    /* legacy 兼容帧（无 CMD: 前缀） */
    if (!(n >= 4 && s[0]=='C' && s[1]=='M' && s[2]=='D' && s[3]==':')) {
        idbuf[0]='0'; idbuf[1]=0; idn=1;
        if (s[0] == 'V') { want_state = 1; queue_ack(idbuf, idn, 0); return; }
        /* legacy 'D' = 进 ISP 下载模式（stcflash 全自动烧录约定，与 demo/探针固件一致）；
           板级诊断走 CMD:<id>:diag，不占用 'D'。 */
        if (s[0] == 'D') { isp_countdown = 5; queue_ack(idbuf, idn, 0); return; }
        if (s[0] == 'B' && n >= 3) {
            unsigned char d1 = (unsigned char)(s[1]-'0'), d2 = (unsigned char)(s[2]-'0');
            if (d1 > 9) d1 = 0; if (d2 > 9) d2 = 0;
            cd = do_beep(freq_tbl[d1], dur_tbl[d2]);
            queue_ack(idbuf, idn, cd); return;
        }
        if (s[0] == 'L' && n >= 2) {
            unsigned char d = (unsigned char)(s[1]-'0');
            if (d > 9) d = 0;
            do_led((d == 0) ? 0 : (d == 9) ? 0xFF : (unsigned int)(1 << (d - 1)));
            queue_ack(idbuf, idn, 0); return;
        }
        if (s[0] == 'N' && n >= 9) { queue_ack(idbuf, idn, do_display(s, 1, 9)); return; }
        if (s[0] == 'T' && n >= 5) {
            sync_buf[0]=s[1]; sync_buf[1]=s[2]; sync_buf[2]=s[3]; sync_buf[3]=s[4];
            sync_buf[4]='0'; sync_buf[5]='0'; apply_sync(); want_state = 1; queue_ack(idbuf, idn, 0); return;
        }
        queue_ack(idbuf, idn, 3); return;
    }
    /* CMD:<id>:<verb>[:args] */
    p1 = findch(s, n, ':', 4); if (p1 == 0xFF) { queue_ack("?", 1, 1); return; }
    p2 = findch(s, n, ':', (unsigned char)(p1 + 1)); if (p2 == 0xFF) p2 = n;
    idn = (unsigned char)(p1 - 4); if (idn > 11) idn = 11;
    for (i = 0; i < idn; i++) idbuf[i] = s[4 + i];
    idbuf[idn] = 0;
    cd = exec_cmd(s + p1 + 1, (unsigned char)(p2 - p1 - 1),
                    (p2 == n) ? s + n : s + p2 + 1,
                    (p2 == n) ? 0 : (unsigned char)(n - p2 - 1));
    queue_ack(idbuf, idn, cd);
}


/* ---------------- 100mS 拍：事件消费 + 命令执行 + 发送窗口 ---------------- */
void cb100ms(void)
{
    unsigned char h, v, k, ka, sent = 0;

    h = GetHallAct();
    if (h == enumHallGetClose) ev_hall = 1;
    else if (h == enumHallGetAway) ev_hall = 2;
    v = GetVibAct();
    if (v == enumVibQuake) ev_vib = 1;
    for (k = 0; k < 3; k++) {
        ka = GetKeyAct((char)k);
        if (ka == enumKeyPress) ev_key = (unsigned char)(k + 1) | 0x10;
        else if (ka == enumKeyRelease) ev_key = (unsigned char)(k + 1) | 0x20;
    }
    ka = GetAdcNavAct(enumAdcNavKeyRight);
    if (ka == enumKeyPress) { ev_nav = 0x11; nav_state = 1; }
    else if (ka == enumKeyRelease) { ev_nav = 0x21; if (nav_state == 1) nav_state = 0; }
    ka = GetAdcNavAct(enumAdcNavKeyDown);
    if (ka == enumKeyPress) { ev_nav = 0x12; nav_state = 2; }
    else if (ka == enumKeyRelease) { ev_nav = 0x22; if (nav_state == 2) nav_state = 0; }
    ka = GetAdcNavAct(enumAdcNavKeyCenter);
    if (ka == enumKeyPress) { ev_nav = 0x13; nav_state = 3; }
    else if (ka == enumKeyRelease) { ev_nav = 0x23; if (nav_state == 3) nav_state = 0; }
    ka = GetAdcNavAct(enumAdcNavKeyLeft);
    if (ka == enumKeyPress) { ev_nav = 0x14; nav_state = 4; }
    else if (ka == enumKeyRelease) { ev_nav = 0x24; if (nav_state == 4) nav_state = 0; }
    ka = GetAdcNavAct(enumAdcNavKeyUp);
    if (ka == enumKeyPress) { ev_nav = 0x15; nav_state = 5; }
    else if (ka == enumKeyRelease) { ev_nav = 0x25; if (nav_state == 5) nav_state = 0; }
    ka = GetAdcNavAct(enumAdcNavKey3);
    if (ka == enumKeyPress) { ev_nav = 0x16; nav_state = 6; key3_state = 1; }
    else if (ka == enumKeyRelease) { ev_nav = 0x26; if (nav_state == 6) nav_state = 0; key3_state = 0; }

    if (line_ready) { line_ready = 0; process_line(); }

    /* 蜂鸣必须先实际执行；ACK 等蜂鸣结束后再发送，避免把排队/串口写入冒充成功。 */
    if (beep_pending && GetBeepStatus() == enumBeepFree && GetUart1TxStatus() == enumUart1TxFree) {
        if (SetBeep(beep_freq, beep_dur) != enumSetBeepOK) ack_code = 2;
        beep_pending = 0;
    }

    if (GetUart1TxStatus() == enumUart1TxFree && GetBeepStatus() == enumBeepFree) {
        if (ev_hall) { send_event((ev_hall == 1) ? "hall=close" : "hall=away"); ev_hall = 0; sent = 1; }
        else if (ev_vib) { send_event("vib=quake"); ev_vib = 0; sent = 1; }
        else if (ev_key) {
            lbuf[0]='k'; lbuf[1]='e'; lbuf[2]='y'; lbuf[3]=(char)('0' + (ev_key & 0x0F)); lbuf[4]='=';
            lbuf[5]=(ev_key & 0x20) ? 'r' : 'p';
            lbuf[6]=(ev_key & 0x20) ? 'e' : 'r';
            lbuf[7]=(ev_key & 0x20) ? 'l' : 'e';
            lbuf[8]=(ev_key & 0x20) ? 'e' : 's';
            lbuf[9]=(ev_key & 0x20) ? 'a' : 's';
            lbuf[10]=(ev_key & 0x20) ? 's' : 0;
            lbuf[11]=(ev_key & 0x20) ? 'e' : 0;
            lbuf[12]=0;
            send_event(lbuf); ev_key = 0; sent = 1;
        }
        else if (ev_nav) {
            unsigned char p = 0, id = (unsigned char)(ev_nav & 0x0F), rel = (unsigned char)(ev_nav & 0x20);
            if (id == 6) { lbuf[p++]='k'; lbuf[p++]='e'; lbuf[p++]='y'; lbuf[p++]='3'; }
            else { lbuf[p++]='n'; lbuf[p++]='a'; lbuf[p++]='v'; lbuf[p++]='='; lbuf[p++]=(char)('0'+id); }
            lbuf[p++]=':';
            if (rel) { lbuf[p++]='r'; lbuf[p++]='e'; lbuf[p++]='l'; lbuf[p++]='e'; lbuf[p++]='a'; lbuf[p++]='s'; lbuf[p++]='e'; }
            else { lbuf[p++]='p'; lbuf[p++]='r'; lbuf[p++]='e'; lbuf[p++]='s'; lbuf[p++]='s'; }
            lbuf[p]=0; send_event(lbuf); ev_nav = 0; sent = 1;
        }
        else if (ack_code != 0xFF && !(motor_ack_pending && GetStepMotorStatus(enumStepMotor1) == enumStepMotorBusy)) {
            motor_ack_pending = 0; send_ack(); sent = 1;
        }
        else if (want_state) {
            want_state = 0;
            send_state(); sent = 1;
        }
    }
}

/* ---------------- 1S 拍：软件 hour + 自动 STATE ---------------- */
void cb1s(void)
{
    sw_sec = bcd_inc(sw_sec, 0x59);
    if (sw_sec == 0x00) {
        sw_min = bcd_inc(sw_min, 0x59);
        if (sw_min == 0x00) sw_hour = bcd_inc(sw_hour, 0x23);
    }
    if (display_clock) show_clock(sw_min, sw_sec);
    want_state = 1;

    /* ISP 倒计时：给上位机时间启动 stcgal 握手，到点软复位进 bootloader。
       实测(2026-09-02)：立即复位会落回用户区，延迟复位 + 已就位握手流才稳定接住。 */
    if (isp_countdown != 0) {
        isp_countdown--;
        if (isp_countdown == 0) {
            IAP_CONTR = 0xE0;              /* IAPEN|SWBS|SWRST -> ISP */
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
    StepMotorInit();

    /* 霍尔引脚 P1.2 必须是数字输入：AdcInit 之后显式设高阻输入，杜绝被 ADC 模式占用 */
    P1M1 |= 0x04;
    P1M0 &= ~0x04;

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
    sw_min = t.minute;
    sw_sec = t.second;

    SetUart1Rxd(&rxbuf, 1, 0, 0);
    SetEventCallBack(enumEventSys100mS, cb100ms);
    SetEventCallBack(enumEventSys1S, cb1s);
    SetEventCallBack(enumEventUart1Rxd, cbrx);

    MySTC_Init();
    send_line(TAG_BOOT);
    send_line(TAG_CAPS);
    while (1) { MySTC_OS(); }
}
