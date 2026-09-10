#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""STC-B 传感器+执行器探针固件 编译/烧录：python build.py [--flash|-f] [--flash-only|-F]
本探针无药盒调度逻辑，单模块 main.c，绕开 demo 药盒 build 的 restricted-Keil 2KB 限制。
默认只编译；--flash 会打断正在使用该板的上位机，执行前先释放串口。
"""
import argparse, os, re, subprocess, sys

if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

KEIL_HOME = os.environ.get("KEIL_HOME", r"C:\Keil_v5")
KEIL_BIN = os.path.join(KEIL_HOME, "C51", "BIN")
C51 = os.path.join(KEIL_BIN, "C51.exe")
BL51 = os.path.join(KEIL_BIN, "BL51.exe")
OH51 = os.path.join(KEIL_BIN, "OH51.exe")
STC_INC = os.path.join(KEIL_HOME, "C51", "INC", "STC")
COM_PORT = os.environ.get("STC_PORT", "COM3")

BASE = os.path.dirname(os.path.abspath(__file__))
BSP_INC = os.path.abspath(os.path.join(BASE, "..", "..", "BSP", "inc"))
BSP_LIB = os.path.abspath(os.path.join(BASE, "..", "..", "BSP", "STCBSP_V3.6.LIB"))


def compile_main():
    r = subprocess.run([C51, os.path.join(BASE, "main.c"), "DB", "OE", "BR",
                        f"INCDIR({BSP_INC};{STC_INC})"],
                       capture_output=True, cwd=BASE, timeout=40)
    out = r.stdout.decode("latin-1", errors="replace")
    if not re.search(r"\b0\s+ERROR\(S\)", out, re.IGNORECASE):
        print("[FAIL] C51")
        for line in out.splitlines():
            if "ERROR" in line.upper():
                print("  " + line.strip())
        return False
    r = subprocess.run([BL51, os.path.join(BASE, "main.obj") + "," + BSP_LIB,
                        "TO", os.path.join(BASE, "main")],
                       capture_output=True, cwd=BASE, timeout=40)
    if r.returncode > 1:
        print("[FAIL] BL51")
        print(r.stdout.decode("latin-1", errors="replace")[-400:])
        return False
    hexfile = os.path.join(BASE, "main.hex")
    if os.path.exists(hexfile):
        os.remove(hexfile)
    subprocess.run([OH51, os.path.join(BASE, "main")], capture_output=True, cwd=BASE, timeout=40)

    if not os.path.exists(hexfile):
        print("[FAIL] OH51 (no hex)")
        return False
    print(f"[OK] main.hex ({os.path.getsize(hexfile)} bytes)")
    return True


def flash_main():
    hexfile = os.path.join(BASE, "main.hex")
    if not os.path.exists(hexfile):
        print("[FAIL] 先编译")
        return False
    sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools")))
    import stcflash
    ok = stcflash.flash(hexfile, port=COM_PORT, label="sensor-probe", baud=115200)
    print("Flash OK" if ok else "Flash FAIL")
    return ok


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="STC-B 探针固件 编译/烧录")
    ap.add_argument("--flash", "-f", action="store_true")
    ap.add_argument("--flash-only", "-F", action="store_true")
    args = ap.parse_args()
    if args.flash_only:
        sys.exit(0 if flash_main() else 1)
    ok = compile_main()
    if ok and args.flash:
        ok = flash_main()
    sys.exit(0 if ok else 1)
