#!/usr/bin/env python3
r"""Build/flash STC-B IAP self-programming probe.

Usage: python build.py [--flash|-f] [--flash-only|-F] [--test]
Environment: KEIL_HOME (default C:\Keil_v5), STC_PORT (default COM3).

Safety gate: the probe erases the flash sector at TEST_ADDR (0xE000). The datasheet
warns "不要将自己的有效程序擦除", so the build fails if linked code size reaches
that address. Keep this gate if you reuse the probe.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys

if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

BASE = pathlib.Path(__file__).resolve().parent
ROOT = BASE.parents[1]
KEIL_HOME = pathlib.Path(os.environ.get("KEIL_HOME", r"C:\Keil_v5"))
KEIL_BIN = KEIL_HOME / "C51" / "BIN"
C51 = str(KEIL_BIN / "C51.exe")
BL51 = str(KEIL_BIN / "BL51.exe")
OH51 = str(KEIL_BIN / "OH51.exe")
STC_INC = str(KEIL_HOME / "C51" / "INC" / "STC")
BSP_INC = str(ROOT / "BSP" / "inc")
BSP_LIB = str(ROOT / "BSP" / "STCBSP_V3.6.LIB")
PORT = os.environ.get("STC_PORT", "COM3")

# 与 main.c 的 IAP_TEST_ADDR 保持一致：探针代码绝不能长到这个地址
TEST_ADDR = 0xE000


def build() -> bool:
    result = subprocess.run(
        [C51, str(BASE / "main.c"), "DB", "OE", "BR", f"INCDIR({BSP_INC};{STC_INC})"],
        cwd=BASE, capture_output=True, timeout=40,
    )
    output = result.stdout.decode("latin-1", errors="replace")
    if not re.search(r"\b0\s+ERROR\(S\)", output, re.IGNORECASE):
        print("[FAIL] C51")
        print("\n".join(line for line in output.splitlines() if "ERROR" in line.upper()))
        return False

    map_path = BASE / "main.M51"
    map_path.unlink(missing_ok=True)

    result = subprocess.run(
        [BL51, f"{BASE / 'main.obj'},{BSP_LIB}", "TO", str(BASE / "main")],
        cwd=BASE, capture_output=True, timeout=40,
    )
    if result.returncode > 1:
        print("[FAIL] BL51")
        print(result.stdout.decode("latin-1", errors="replace")[-500:])
        return False

    hex_path = BASE / "main.hex"
    hex_path.unlink(missing_ok=True)
    subprocess.run([OH51, str(BASE / "main")], cwd=BASE, capture_output=True, timeout=40)
    if not hex_path.exists():
        print("[FAIL] OH51: main.hex not produced")
        return False

    if not map_path.exists():
        print("[FAIL] link map main.M51 missing; cannot enforce the code-size gate")
        return False
    map_text = map_path.read_text(encoding="latin-1", errors="replace")
    match = re.search(r"Program Size:.*?\bcode=(\d+)", map_text, re.IGNORECASE)
    if not match:
        print("[FAIL] cannot parse 'code=' from main.M51; code-size gate cannot run")
        return False
    code_bytes = int(match.group(1))
    if code_bytes >= TEST_ADDR:
        print(f"[FAIL] code={code_bytes} reaches the IAP test sector at 0x{TEST_ADDR:04X}; "
              "the probe would erase its own program (move TEST_ADDR up or shrink the code)")
        return False

    print(f"[OK] {hex_path.name} ({hex_path.stat().st_size} bytes), code={code_bytes} "
          f"({0xE000 - code_bytes} bytes below the 0x{TEST_ADDR:04X} test sector)")
    return True


def flash() -> bool:
    sys.path.insert(0, str(ROOT / "tools"))
    import stcflash
    # 板上常驻固件的 UART 是 115200；stcflash 发 D 命令必须用同一波特率
    ok = stcflash.flash(str(BASE / "main.hex"), port=PORT, label="iap-probe", baud=115200)
    print("Flash OK" if ok else "Flash FAIL")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description="Build/flash STC-B IAP probe")
    parser.add_argument("--flash", "-f", action="store_true", help="build then flash")
    parser.add_argument("--flash-only", "-F", action="store_true", help="flash existing hex")
    args = parser.parse_args()
    if args.flash_only:
        return 0 if flash() else 1
    if not build():
        return 1
    if args.flash and not flash():
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
