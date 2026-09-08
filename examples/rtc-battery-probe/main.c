/* RTC 纽扣电池消融探针（rtc-battery-probe）
 *
 * 一次性回答一个问题：**STC-B 拔掉 USB 之后，DS1302 的钟还走不走？**
 *
 * 为什么要专门做这个探针（矛盾点）：
 *   官方 BSP 头文件 DS1302.h 写的是——「断电后，RTC 和 NVM 是依靠纽扣电池 BAT 维持
 *   工作的」，且 DS1302Init(time) 只在「初始化时检测 RTC 数据失效（掉电）」时才用参数
 *   时间重置 RTC。
 *   但课程期板测记录写的是——「掉电检测 NVM[30] 不可靠，长时间断电后上电 RTC 被
 *   DS1302Init 重置为初始化参数」。
 *   这两条互相矛盾，而且从来没有做过受控实验去区分下面三种完全不同的原因：
 *     A. 电池有效、RTC 断电继续走：所谓「被重置」其实是固件每次上电无条件调用
 *        DS1302Init(默认时间) 自己把活着的钟冲掉了（自伤，不是硬件问题）；
 *     B. 电池有效但 RTC 停振：NVM（同样是电池保持的 RAM）内容还在，RTC 不走；
 *     C. 电池没装 / 没电：NVM 与 RTC 一起丢。
 *   A 与 C 的修法完全相反：A 只要固件别乱初始化就能白捡一个断电走时的钟；
 *   C 则必须靠上位机对时 + 可选 M24C02 存「最后已知时间」。
 *
 * 消融设计（第一性原理：先把「我们自己造成的破坏」从实验里拿掉）：
 *   1. 启动路径**绝不调用 DS1302Init**——它会按掉电检测决定是否覆盖 RTC，一调用就毁掉
 *      本次要观察的状态。只有显式按 I 才做对照实验。
 *   2. 上电后第一件事就是原样读 RTC 与 NVM 标记，打印 PRE: 行，并把芯片时间直接显示在
 *      数码管上（不需要串口也能看出钟在不在走）。
 *   3. NVM[21]=启动计数、NVM[22]/[23]=0xA5/0x5A 标记：NVM 与 RTC 都由电池保持，
 *      但 NVM 不依赖振荡器。两者分开判读才能把 B 从 A/C 里区分出来。
 *   4. 已知本板 DS1302 的 hour 寄存器会自发变成 bit7 置位的垃圾值（课程期实测），
 *      因此判读一律以 min/sec 为准，hour 只作为原始证据打印（hourraw / hourbad）。
 *
 * 命令（单字符，115200 8N1）：
 *   H 帮助  R 原样读一次  S 写 RTC=12:34:56 + 写 NVM 标记  N 只写 NVM 标记
 *   I 对照：调用 DS1302Init(08:00:00) 后再读（**会破坏实验状态，最后才用**）
 *   T 连续 10 拍打印芯片时间（证明振荡器在跑）
 *   D 5 秒后软复位进 ISP（stcflash 全自动烧录约定）
 *
 * 判读表见 README.md。
 */
#include "STC15F2K60S2.H"
#include "sys.H"
#include "displayer.H"
#include "uart1.h"
#include "DS1302.h"

code unsigned long SysClock = 11059200;

#ifdef _displayer_H_
code char decode_table[] = {
    0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f,
    0x00,0x08,0x40,0x01,0x76,0x38,
    0x3f|0x80,0x06|0x80,0x5b|0x80,0x4f|0x80,0x66|0x80,
    0x6d|0x80,0x7d|0x80,0x07|0x80,0x7f|0x80,0x6f|0x80
};
#endif

/* NVM 布局：避开 addr30（DS1302Init 的掉电检测单元，用户不可用）与
   diag-rtc 探针用过的 addr20，也不碰大作业固件的配置区 0x00-0x07。 */
#define NVM_BOOTS 21
#define NVM_MARK1 22
#define NVM_MARK2 23
#define MARK1_VAL 0xA5
#define MARK2_VAL 0x5A

static xdata char obuf[128];
static xdata struct_DS1302_RTC chip;
static char rxbuf;
static unsigned char tick_left;
static unsigned char tick_n;
static unsigned char isp_countdown;

static char hexn(unsigned char v)
{
    v &= 0x0F;
    return (v < 10) ? (char)('0' + v) : (char)('A' + v - 10);
}

/* 发送统一走 obuf：调用方只填内容，这里补 CRLF 并阻塞等 TX 空闲（探针必须每条都发出去）。 */
static void send_bytes(unsigned char n)
{
    obuf[n] = '\r'; obuf[n + 1] = '\n';
    while (GetUart1TxStatus() != enumUart1TxFree);
    Uart1Print(obuf, (unsigned char)(n + 2));
}

static void send_line(char code *s)
{
    unsigned char i = 0;
    while (s[i] != 0 && i < 120) { obuf[i] = s[i]; i++; }
    send_bytes(i);
}

static void pstr(unsigned char *i, char code *s)
{
    while (*s != 0 && *i < 120) obuf[(*i)++] = *s++;
}

static void phex2(unsigned char *i, unsigned char v)
{
    if (*i > 118) return;
    obuf[(*i)++] = hexn((unsigned char)(v >> 4));
    obuf[(*i)++] = hexn(v);
}

/* 数码管直接显示芯片 RTC 的 HH-MM-SS（d0=最左；码 12 = '-'）。
   hour 若是垃圾值也照实显示，肉眼就能看出本板 hour 寄存器的老毛病。 */
static void show_chip(void)
{
    Seg7Print((unsigned char)(chip.hour >> 4),   (unsigned char)(chip.hour & 0x0F),   12,
              (unsigned char)(chip.minute >> 4), (unsigned char)(chip.minute & 0x0F), 12,
              (unsigned char)(chip.second >> 4), (unsigned char)(chip.second & 0x0F));
}

/* 原样上报：RTC + NVM 标记 + 启动计数。tag 用 code 指针（只传 ROM 字面量，
   绝不传 xdata 缓冲——那正是 stcb-full 早期读出 flash 垃圾字节的雷）。 */
static void report(char code *tag)
{
    unsigned char i = 0, m1, m2, bt;

    chip = RTC_Read();
    m1 = NVM_Read(NVM_MARK1);
    m2 = NVM_Read(NVM_MARK2);
    bt = NVM_Read(NVM_BOOTS);

    pstr(&i, tag);
    pstr(&i, "rtc=");
    phex2(&i, chip.hour);   obuf[i++] = ':';
    phex2(&i, chip.minute); obuf[i++] = ':';
    phex2(&i, chip.second);
    pstr(&i, ",hourraw="); phex2(&i, chip.hour);
    pstr(&i, ",hourbad="); obuf[i++] = (char)('0' + ((chip.hour & 0x80) ? 1 : 0));
    pstr(&i, ",ymd=");
    phex2(&i, chip.year);  obuf[i++] = '-';
    phex2(&i, chip.month); obuf[i++] = '-';
    phex2(&i, chip.day);
    pstr(&i, ",w="); phex2(&i, chip.week);
    pstr(&i, ",boots="); phex2(&i, bt);
    pstr(&i, ",mark="); phex2(&i, m1); obuf[i++] = ','; phex2(&i, m2);
    pstr(&i, ",markok=");
    obuf[i++] = (char)('0' + ((m1 == MARK1_VAL && m2 == MARK2_VAL) ? 1 : 0));
    send_bytes(i);
}

/* S：写入一个显眼的时间 + NVM 标记，作为断电前后的对照基准。 */
static void do_set(void)
{
    struct_DS1302_RTC w;
    unsigned char i = 0;

    w.second = 0x56; w.minute = 0x34; w.hour = 0x12;
    w.day = 0x04; w.month = 0x09; w.week = 0x05; w.year = 0x26;
    RTC_Write(w);
    NVM_Write(NVM_MARK1, MARK1_VAL);
    NVM_Write(NVM_MARK2, MARK2_VAL);

    chip = RTC_Read();
    pstr(&i, "SET:want=12:34:56,got=");
    phex2(&i, chip.hour);   obuf[i++] = ':';
    phex2(&i, chip.minute); obuf[i++] = ':';
    phex2(&i, chip.second);
    pstr(&i, ",mark=");
    phex2(&i, NVM_Read(NVM_MARK1)); obuf[i++] = ',';
    phex2(&i, NVM_Read(NVM_MARK2));
    pstr(&i, ",minok=");
    obuf[i++] = (char)('0' + ((chip.minute == 0x34) ? 1 : 0));
    send_bytes(i);
}

/* N：只写 NVM 标记，不碰 RTC（单独验证电池是否给 NVM 供电）。 */
static void do_marks(void)
{
    unsigned char i = 0;
    NVM_Write(NVM_MARK1, MARK1_VAL);
    NVM_Write(NVM_MARK2, MARK2_VAL);
    pstr(&i, "MARKS:want=A5,5A,got=");
    phex2(&i, NVM_Read(NVM_MARK1)); obuf[i++] = ',';
    phex2(&i, NVM_Read(NVM_MARK2));
    send_bytes(i);
}

/* I：对照实验——调用 BSP 的 DS1302Init(08:00:00)，看它到底会不会把活着的钟冲掉。
   这一步会破坏实验状态，必须在断电实验做完之后才用。 */
static void do_init(void)
{
    struct_DS1302_RTC d;
    d.second = 0x00; d.minute = 0x00; d.hour = 0x08;
    d.day = 0x02; d.month = 0x09; d.week = 0x03; d.year = 0x26;
    DS1302Init(d);
    report("POST:");
}

/* 板级红线（本探针 v2 实测）：不要在 enumEventSys1S 回调里做「阻塞等 TX 空闲 +
   DS1302 位bang 读」的组合——v2 加了一条每 5 秒的 LIVE 心跳后固件直接挂死
   （串口全哑，R/H 都不应答）。时间轴证据改由主机侧每 5 秒发 R 轮询获取，
   固件保持 v1 已板测的行为。 */
static void cb1s(void)
{
    chip = RTC_Read();
    show_chip();
    if (tick_left != 0) {
        unsigned char i = 0;
        tick_n++;
        pstr(&i, "TICK:"); phex2(&i, tick_n);
        pstr(&i, ",rtc=");
        phex2(&i, chip.hour);   obuf[i++] = ':';
        phex2(&i, chip.minute); obuf[i++] = ':';
        phex2(&i, chip.second);
        send_bytes(i);
        tick_left--;
    }
    /* ISP 倒计时：给上位机时间启动 stcgal 握手，到点软复位进 bootloader
       （实测：立即复位会落回用户区，延迟复位才稳定接住）。 */
    if (isp_countdown != 0) {
        isp_countdown--;
        if (isp_countdown == 0) {
            send_line("ISP:reset-now");
            IAP_CONTR = 0xE0;              /* IAPEN|SWBS|SWRST -> ISP */
            while (1);
        }
    }
}

static void cbrx(void)
{
    switch ((unsigned char)rxbuf) {
        case 'H': send_line("HELP:H R=read S=set12:34:56+marks N=marks I=DS1302Init(compare) T=10ticks D=isp"); break;
        case 'R': report("READ:"); break;
        case 'S': do_set(); break;
        case 'N': do_marks(); break;
        case 'I': do_init(); break;
        case 'T': tick_n = 0; tick_left = 10; break;
        case 'D': isp_countdown = 5; send_line("ISP:countdown=5s"); break;
        default: break;   /* \r \n 与未知字符一律忽略 */
    }
}

void main(void)
{
    unsigned char bt;

    DisplayerInit();
    SetDisplayerArea(0, 7);
    Seg7Print(10, 10, 10, 10, 10, 10, 10, 10);
    Uart1Init(115200);

    /* 上电第一件事：原样读（对照组）。实测本板不 Init 时三wire总线未被驱动，
       读回来是浮空垃圾（rtc=6E:79:73 / mark=FE,F8），所以 PRE 只作为「未驱动」的
       对照证据，不能拿来判断电池。 */
    report("PRE:");

    /* 启动计数 +1：NVM 是否跨断电保存的独立证据（与 RTC 分开判读）。 */
    bt = (unsigned char)(NVM_Read(NVM_BOOTS) + 1);
    NVM_Write(NVM_BOOTS, bt);

    /* 再初始化并复读：Init 清 CH 起振（BSP 头文件明写「需初始化和驱动一次」）。
       本板实测 Init **不会**用 init_time 覆盖 RTC（日期停在 00-01-01），
       因此 POST 就是芯片断电期间真正保留下来的状态。 */
    {
        struct_DS1302_RTC d;
        d.second = 0x00; d.minute = 0x00; d.hour = 0x08;
        d.day = 0x02; d.month = 0x09; d.week = 0x03; d.year = 0x26;
        DS1302Init(d);
    }
    report("POST:");

    SetUart1Rxd(&rxbuf, 1, 0, 0);
    SetEventCallBack(enumEventSys1S, cb1s);
    SetEventCallBack(enumEventUart1Rxd, cbrx);
    MySTC_Init();

    send_line("BOOT:probe=rtc-battery-v3,uart=115200,seq=PRE-raw,Init,POST,poll-from-host");
    send_line("HELP:H R=read S=set12:34:56+marks N=marks I=DS1302Init(compare) T=10ticks D=isp");
    chip = RTC_Read();
    show_chip();
    while (1) { MySTC_OS(); }
}
