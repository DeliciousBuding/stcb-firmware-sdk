/* SPDX-License-Identifier: Apache-2.0 */

/* STC-B IAP 自编程探针（iap-probe v0.1）
 *
 * 目的：在真板上证明 IAP15F2K61S2 能在「用户程序区」擦 / 写 / 读自己的 Flash。
 * 这是「设备自写固件更新（真 OTA）」的最小地基：在拿到 PASS 之前，不要设计传输协议、
 * 不要动 edge 执行器——一个探针就决定整条路走不走得通。
 *
 * 数据手册依据（STC15F2K60S2 数据手册第 730 页 EEPROM 选型表）：
 *   IAP15F2K61S2 / IAP15L2K61S2：EEPROM 字节数「-」，扇区数 122，
 *   用 IAP 字节读时地址范围 0000h..F3FFh，
 *   备注「没有专门的 EEPROM，但可在用户程序区修改用户程序，使用时不要将自己的有效程序擦除」。
 *   即：IAP 地址空间覆盖用户程序区 `0000h..F3FFh`，不存在独立的 EEPROM 区。
 *   本探针只在 `0xE000` 单扇区实测；这不等于已逐扇区扫描全片。
 *
 * 寄存器（同手册 9.1 节）：
 *   IAP_CMD   MS1:MS0 = 00 待机 / 01 字节读 / 10 字节编程 / 11 扇区擦除（扇区 512 字节）
 *   IAP_TRIG  先写 5Ah 再写 A5h 才生效；每次触发前需重送命令
 *   IAP_CONTR bit7 IAPEN=1 使能；bit4 CMD_FAIL=1 表示地址非法，需软件写 0 清零
 *   IAP_CONTR WT2:WT1:WT0 等待时间：011 = 推荐系统时钟 <=12MHz（本板 11.0592MHz）
 *
 * 安全约束：测试扇区必须远离探针自身的有效程序——build.py 有代码体积门禁，
 *   编译时若 code 逼近 IAP_TEST_ADDR 会直接失败（手册原话：不要将自己的有效程序擦除）。
 *
 * 板级注记：IAP 期间关中断（手册 16.7），单次扇区擦除约 21ms，会打断 BSP 的 1ms 调度；
 *   本探针只在收到 I 命令时执行一次，属于 bring-up 工具，不承担实时业务。
 *   全程不发声（探针无 SetBeep 调用），因此不存在「同拍蜂鸣损坏 UART TX」的雷。
 *
 * 命令（ASCII 单字节；CR/LF 被忽略）：
 *   I -> 跑一次 IAP 自测，逐条上报 IAP:RESULT:*
 *   S -> 重发上次自测结果
 *   D -> 延迟 12s 软复位进 ISP（烧录链红线：常驻固件必须保留 D）
 */

#include "STC15F2K60S2.H"
#include "sys.H"
#include "displayer.h"
#include "uart1.h"
#include "Beep.h"
#include "Key.H"
#include "adc.h"
#include "Vib.h"
#include "hall.H"

code unsigned long SysClock = 11059200;

code char TAG_BOOT[] = "HELLO:iap-probe:v0.1:baud=115200";

/* ---- IAP 常量（数据手册 9.1 / 9.2.2）---- */
#define IAP_CMD_STANDBY 0x00
#define IAP_CMD_READ    0x01
#define IAP_CMD_PROGRAM 0x02
#define IAP_CMD_ERASE   0x03

#define IAP_CONTR_ENABLE  0x83   /* IAPEN=1, WT2:WT0=011 -> 推荐系统时钟 <=12MHz */
#define IAP_CONTR_CMDFAIL 0x10   /* bit4：地址非法时置位，需软件清零 */

#define IAP_SECTOR_BYTES  512U
#define IAP_ADDR_ILLEGAL  0xF400U  /* 合法空间到 F3FFh 为止，F400h 起应判非法 */
#define IAP_TEST_ADDR     0xE000U  /* 第 112/122 扇区：远高于探针有效程序 */
#define IAP_PATTERN_LEN   16U

/* 覆盖扇区首尾：16 字节模式 + 扇区末字节，避免「只测了第一个字节」的假阳性 */
code unsigned char IAP_PATTERN[IAP_PATTERN_LEN] = {
    0x5A, 0xA5, 0x00, 0xFF, 0x11, 0x22, 0x33, 0x44,
    0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC
};
code unsigned char IAP_TAIL_VALUE = 0x7E;

char rxbuf;

static unsigned char iap_request = 0;
static unsigned char dump_request = 0;
static unsigned char isp_countdown = 0;

/* 上一次自测结果（S 命令可重发；数码管显示 PASS/FAIL 供无 PC 时肉眼判读） */
static unsigned char res_ran = 0;
static unsigned char res_read_pre_blank = 0;
static unsigned char res_program_cmdfail = 1;
static unsigned char res_read_post_match = 0;
static unsigned char res_erase_cmdfail = 1;
static unsigned char res_read_after_blank = 0;
static unsigned char res_cmdfail_raised = 0;
static unsigned char res_total_pass = 0;
static unsigned int  res_read_pre_dirty = 0;
static unsigned int  res_read_after_dirty = 0;

static xdata unsigned char outbuf[96];
static unsigned char outn = 0;
static xdata unsigned char readback[IAP_PATTERN_LEN];

/* ---------------- 串口行输出 ---------------- */

static void out_reset(void)
{
    /* 关键：Uart1Print 直接引用 outbuf（不拷贝数据）。若上一行仍在发送时就改写缓冲，
       上一行的尾部会被本行内容替换，实测表现为两行交错/粘连（2026-09-10 首轮踩到）。
       因此动 outbuf 之前必须等发送空闲——这是本探针唯一的串口纪律。 */
    while (GetUart1TxStatus() != enumUart1TxFree) { }
    outn = 0;
}

static void out_char(char c)
{
    if (outn < 92) outbuf[outn++] = (unsigned char)c;
}

static void out_lit(char code *s)
{
    unsigned char i = 0;
    while (s[i] != 0 && outn < 92) outbuf[outn++] = (unsigned char)s[i++];
}

static void out_hex8(unsigned char v)
{
    unsigned char hi = (unsigned char)(v >> 4);
    unsigned char lo = (unsigned char)(v & 0x0F);
    out_char((hi < 10) ? (char)('0' + hi) : (char)('A' + hi - 10));
    out_char((lo < 10) ? (char)('0' + lo) : (char)('A' + lo - 10));
}

static void out_udec(unsigned int v)
{
    unsigned char digits[5];
    unsigned char n = 0;
    if (v == 0) { out_char('0'); return; }
    while (v > 0 && n < 5) {
        digits[n++] = (unsigned char)('0' + (v % 10));
        v = (unsigned int)(v / 10);
    }
    while (n > 0) { n--; out_char((char)digits[n]); }
}

/* Uart1Print 非阻塞：串口忙时返回失败且不会发送，必须重试到 enumUart1TxOK */
static void out_flush(void)
{
    char status;

    outbuf[outn++] = '\r';
    outbuf[outn++] = '\n';
    do {
        while (GetUart1TxStatus() != enumUart1TxFree) { }
        status = Uart1Print(outbuf, outn);
    } while (status != enumUart1TxOK);
    outn = 0;
}

/* ---------------- IAP 原语 ---------------- */

/* 一次 IAP 操作。read_value 非空时回填 IAP_DATA；返回 1 表示 CMD_FAIL（地址非法）。 */
static unsigned char iap_op(unsigned int addr, unsigned char cmd,
                            unsigned char write_value, unsigned char *read_value)
{
    bit ea_state;
    unsigned char fail;

    ea_state = EA;
    EA = 0;   /* 手册 16.7：IAP 操作期间避免中断进入并执行 Flash 相关代码 */

    IAP_CONTR = IAP_CONTR_ENABLE;
    IAP_CMD   = cmd;
    IAP_ADDRH = (unsigned char)(addr >> 8);
    IAP_ADDRL = (unsigned char)(addr & 0x00FF);
    IAP_DATA  = write_value;
    IAP_TRIG  = 0x5A;
    IAP_TRIG  = 0xA5;

    if (read_value != 0) *read_value = IAP_DATA;
    fail = (IAP_CONTR & IAP_CONTR_CMDFAIL) ? 1 : 0;
    if (fail) IAP_CONTR = IAP_CONTR_ENABLE;   /* 写 0 到 CMD_FAIL 清零 */

    IAP_CMD   = IAP_CMD_STANDBY;
    IAP_CONTR = 0x00;

    EA = ea_state;
    return fail;
}

/* 全扇区扫描，返回非 0xFF 字节数（0 = 空白） */
static unsigned int iap_count_non_ff(unsigned int base)
{
    unsigned int offset;
    unsigned int dirty = 0;
    unsigned char value;

    for (offset = 0; offset < IAP_SECTOR_BYTES; offset++) {
        iap_op((unsigned int)(base + offset), IAP_CMD_READ, 0x00, &value);
        if (value != 0xFF) dirty++;
    }
    return dirty;
}

/* 自测序列：先证明能写，再证明能擦——擦除只有在「写入过」之后才可证伪 */
static void run_iap_test(void)
{
    unsigned int i;
    unsigned char value;

    /* 步骤 1：写前基线（出厂/上次擦除后应为空白） */
    res_read_pre_dirty = iap_count_non_ff(IAP_TEST_ADDR);
    res_read_pre_blank = (res_read_pre_dirty == 0) ? 1 : 0;

    /* 步骤 2：字节编程（扇区首 16 字节 + 末字节） */
    res_program_cmdfail = 0;
    for (i = 0; i < IAP_PATTERN_LEN; i++) {
        if (iap_op((unsigned int)(IAP_TEST_ADDR + i), IAP_CMD_PROGRAM, IAP_PATTERN[i], 0) != 0)
            res_program_cmdfail = 1;
    }
    if (iap_op((unsigned int)(IAP_TEST_ADDR + IAP_SECTOR_BYTES - 1U),
               IAP_CMD_PROGRAM, IAP_TAIL_VALUE, 0) != 0)
        res_program_cmdfail = 1;

    /* 步骤 3：读回比对 */
    res_read_post_match = 1;
    for (i = 0; i < IAP_PATTERN_LEN; i++) {
        iap_op((unsigned int)(IAP_TEST_ADDR + i), IAP_CMD_READ, 0x00, &value);
        readback[i] = value;
        if (value != IAP_PATTERN[i]) res_read_post_match = 0;
    }

    /* 步骤 4：扇区擦除 + 步骤 5：全扇区确认空白 */
    res_erase_cmdfail = iap_op(IAP_TEST_ADDR, IAP_CMD_ERASE, 0x00, 0);
    res_read_after_dirty = iap_count_non_ff(IAP_TEST_ADDR);
    res_read_after_blank = (res_read_after_dirty == 0) ? 1 : 0;

    /* 步骤 6：越界地址应被判非法，而不是被静默接受 */
    res_cmdfail_raised = iap_op(IAP_ADDR_ILLEGAL, IAP_CMD_READ, 0x00, &value);

    res_total_pass = (res_read_pre_blank
                      && !res_program_cmdfail
                      && res_read_post_match
                      && !res_erase_cmdfail
                      && res_read_after_blank) ? 1 : 0;
    res_ran = 1;
}

/* ---------------- 结果上报 ---------------- */

static void emit_result(void)
{
    unsigned char k;

    if (!res_ran) {
        out_reset();
        out_lit("IAP:RESULT:step=none state=not-run");
        out_flush();
        return;
    }

    out_reset();
    out_lit("IAP:RESULT:step=read_pre state=");
    out_lit(res_read_pre_blank ? "blank" : "dirty");
    out_lit(" nonff=");
    out_udec(res_read_pre_dirty);
    out_flush();

    out_reset();
    out_lit("IAP:RESULT:step=program state=");
    out_lit(res_program_cmdfail ? "cmdfail" : "ok");
    out_flush();

    out_reset();
    out_lit("IAP:RESULT:step=read_post state=");
    out_lit(res_read_post_match ? "match" : "mismatch");
    out_lit(" data=");
    for (k = 0; k < IAP_PATTERN_LEN; k++) out_hex8(readback[k]);
    out_flush();

    out_reset();
    out_lit("IAP:RESULT:step=erase state=");
    out_lit(res_erase_cmdfail ? "cmdfail" : "ok");
    out_flush();

    out_reset();
    out_lit("IAP:RESULT:step=read_after_erase state=");
    out_lit(res_read_after_blank ? "blank" : "dirty");
    out_lit(" nonff=");
    out_udec(res_read_after_dirty);
    out_flush();

    out_reset();
    out_lit("IAP:RESULT:step=cmdfail state=");
    out_lit(res_cmdfail_raised ? "raised" : "not-raised");
    out_lit(" addr=F400");
    out_flush();

    out_reset();
    out_lit("IAP:RESULT:step=total state=");
    out_lit(res_total_pass ? "PASS" : "FAIL");
    out_flush();
}

static void show_result(void)
{
    /* 末位显示 1=自测通过 / 0=未通过 / 空白=未跑过，供现场肉眼判读 */
    Seg7Print(10, 10, 10, 10, 10, 10, 10,
              res_ran ? (res_total_pass ? 1 : 0) : 10);
}

/* ---------------- 事件回调 ---------------- */

void cbrx(void)
{
    unsigned char c = (unsigned char)rxbuf;

    if (c == 'D') {
        isp_countdown = 12;
        out_reset();
        out_lit("ACK:0:ok");
        out_flush();
        return;
    }
    if (c == '\r' || c == '\n') return;
    if (c == 'I') { iap_request = 1; return; }
    if (c == 'S') { dump_request = 1; return; }
}

void cb1s(void)
{
    if (iap_request) {
        iap_request = 0;
        out_reset();
        out_lit("IAP:START:addr=E000:section=112:512bytes");
        out_flush();
        run_iap_test();
        emit_result();
        show_result();
        dump_request = 0;
    }
    if (dump_request) {
        dump_request = 0;
        emit_result();
    }

    /* D：延迟软复位进 ISP。保留该命令是烧录红线——板子常驻此固件时，
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
    DisplayerInit();
    SetDisplayerArea(0, 7);
    Seg7Print(10, 10, 10, 10, 10, 10, 10, 10);

    BeepInit();
    Uart1Init(115200);
    KeyInit();
    AdcInit(ADCincEXT);
    VibInit();
    HallInit();

    SetUart1Rxd(&rxbuf, 1, 0, 0);
    SetEventCallBack(enumEventSys1S, cb1s);
    SetEventCallBack(enumEventUart1Rxd, cbrx);

    MySTC_Init();

    out_reset();
    out_lit(TAG_BOOT);
    out_flush();

    while (1) { MySTC_OS(); }
}
