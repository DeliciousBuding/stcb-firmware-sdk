#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stcflash — 烧录 SSOT（各 build.py / debug-run.py 共用）

流程：释放 COM 口 → 板上有 D 命令固件则全自动（发 D → 固件 ~5s 后 IAP_CONTR=0xE0
软复位进 ISP → stcgal 握手接住，零按键）→ 失败自动降级手动（提示按 Reset）。

板级事实（2026-09-02 实测，细节 SOP-STC-B烧录.md §3.5）：
- DTR/RTS 自动复位电路拉不动（stcgal -a / -A rts + pyserial 20 组合脉冲矩阵全败）
- 立即软复位落回用户区：无握手流时 bootloader 停留窗口极短
- 延迟软复位 + 已就位的 stcgal 握手 = 稳定接住（D 命令机制）
- D 只需存在于“板上当前固件”（demo/探针带 D），目标 hex 无需任何改动
- stcgal 2400 波特握手垃圾长时间灌入可挂死运行中固件 UART（自动流程仅 ~4s 暴露）

CLI: python tools/stcflash.py <hex> [--port COM3] [--manual] [--label 名称]
API: import stcflash; stcflash.flash(hexpath, port="COM3", label="", auto=True) -> bool
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serlink   # 串口线路基建（端口约定/释放）

DEFAULT_PORT = serlink.default_port()   # env STC_PORT > COM3
BAUD = 9600          # 板端固件串口波特率（发 D 命令用；ISP 握手波特率由 stcgal 自理）
AUTO_TIMEOUT = 45    # 自动模式 stcgal 等待秒数；超时判定板上无 D 固件，降级手动


free_port = serlink.free_port   # 兼容别名（旧入口引用过 stcflash.free_port）


def _stcgal_cmd(port, hexpath):
    g = shutil.which("stcgal")   # 本机 stcgal 在 PATH；python3(miniforge) 未装，勿硬编码 python3 -m
    return ([g] if g else ["python3", "-m", "stcgal"]) + ["-p", port, "-P", "stc15", hexpath]


def try_auto(hexpath, port=DEFAULT_PORT, baud=BAUD):
    """D 命令全自动烧录。True=成功；False=需降级手动。"""
    try:
        import serial
    except ImportError:
        print("[stcflash] 无 pyserial，跳过自动模式")
        return False
    try:
        ser = serial.Serial(port, baud, timeout=0.3)
        time.sleep(0.3)
        # D 发 3 次间隔 1.5s：REMIND/MISSED 蜂鸣窗口的 CCP 中断风暴会吞 RX 字节
        #（板级事实：同拍 SetBeep 损坏 UART，2026-09-02 实测 D 单发丢失导致降级）；
        # 重复 D 只会重置固件倒计时，无副作用。
        for i in range(3):
            ser.write(b"D")
            if i < 2:
                time.sleep(1.5)
        # 行结束符：Full Firmware v1 的行协议要看到 CR/LF 才解析（裸 'D' 会停在行缓冲里），
        # 而 demo/探针固件按单字节生效、且已在上面 3s 内消费掉 'D'，因此补 CRLF 对两者都安全。
        ser.write(b"\r\n")
        time.sleep(0.2)
        ser.close()
    except Exception as e:
        print(f"[stcflash] 无法发 D（{e}）")
        return False
    print("[stcflash] D 已发出：若板上固件带 D 命令，~5s 后软复位进 ISP；stcgal 启动握手中...", flush=True)
    try:
        t0 = time.time()
        r = subprocess.run(_stcgal_cmd(port, hexpath), capture_output=True, text=True, timeout=AUTO_TIMEOUT)
        out = r.stdout + r.stderr
        if r.returncode == 0 and "Disconnected" in out:
            print(f"[stcflash] 全自动烧录成功（{time.time()-t0:.0f}s，零按键）", flush=True)
            return True
        print(f"[stcflash] 自动模式未接住（rc={r.returncode}），降级手动", flush=True)
    except subprocess.TimeoutExpired:
        print(f"[stcflash] stcgal {AUTO_TIMEOUT}s 未等到板子（板上固件无 D 命令？），降级手动", flush=True)
    except Exception as e:
        print(f"[stcflash] 自动模式异常 {e}，降级手动", flush=True)
    return False


def manual(hexpath, port=DEFAULT_PORT):
    """手动模式：前台跑 stcgal，出现 Waiting for MCU 后人工按 Reset"""
    print("[stcflash] 手动模式：stcgal 出现 'Waiting for MCU' 后按一次板上 Reset", flush=True)
    r = subprocess.run(_stcgal_cmd(port, hexpath))
    return r.returncode == 0


def flash(hexpath, port=DEFAULT_PORT, label="", auto=True, baud=BAUD):
    """烧录入口（API）。返回 bool。auto=True 先试 D 命令全自动，失败降级手动。"""
    if not os.path.exists(hexpath):
        print(f"[stcflash] hex 不存在: {hexpath}")
        return False
    tag = f" ({label})" if label else ""
    print(f"[stcflash] flash{tag}: {hexpath} @ {port} [{os.path.getsize(hexpath)} bytes]", flush=True)
    free_port()
    time.sleep(0.5)
    ok = (auto and try_auto(hexpath, port, baud)) or manual(hexpath, port)
    print("[stcflash] Flash OK" if ok else "[stcflash] Flash FAIL", flush=True)
    return ok


def main():
    ap = argparse.ArgumentParser(description="烧录 SSOT：D 命令全自动 + 手动降级")
    ap.add_argument("hex", help="hex 文件路径")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--manual", action="store_true", help="跳过自动模式直接手动")
    ap.add_argument("--label", default="", help="日志标签")
    ap.add_argument("--baud", type=int, default=BAUD,
                    help="板端固件波特率（只影响发 D 命令，默认 9600）")
    a = ap.parse_args()
    sys.exit(0 if flash(a.hex, a.port, a.label, auto=not a.manual, baud=a.baud) else 1)


if __name__ == "__main__":
    main()
