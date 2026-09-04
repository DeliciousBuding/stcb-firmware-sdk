#!/usr/bin/env python3
r"""Build/flash STC-B Full Firmware v1.

Usage: python build.py [--flash|-f] [--flash-only|-F]
Environment: KEIL_HOME (default C:\Keil_v5), STC_PORT (default COM3).
"""
from __future__ import annotations

import argparse
import os
import pathlib
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


def build() -> bool:
    result = subprocess.run(
        [C51, str(BASE / "main.c"), "DB", "OE", "BR", f"INCDIR({BSP_INC};{STC_INC})"],
        cwd=BASE, capture_output=True, timeout=40,
    )
    output = result.stdout.decode("latin-1", errors="replace")
    if "0 ERROR" not in output:
        print("[FAIL] C51")
        print("\n".join(line for line in output.splitlines() if "ERROR" in line.upper()))
        return False
    result = subprocess.run(
        [BL51, f"{BASE / 'main.obj'},{BSP_LIB}", "TO", str(BASE / "main")],
        cwd=BASE, capture_output=True, timeout=40,
    )
    if result.returncode > 1:
        print("[FAIL] BL51")
        print(result.stdout.decode("latin-1", errors="replace")[-500:])
        return False
    subprocess.run([OH51, str(BASE / "main")], cwd=BASE, capture_output=True, timeout=40)
    hex_path = BASE / "main.hex"
    if not hex_path.exists():
        print("[FAIL] OH51: main.hex not produced")
        return False
    print(f"[OK] {hex_path.name} ({hex_path.stat().st_size} bytes)")
    return True


def flash() -> bool:
    sys.path.insert(0, str(ROOT / "tools"))
    import stcflash
    ok = stcflash.flash(str(BASE / "main.hex"), port=PORT, label="stcb-full-v1")
    print("Flash OK" if ok else "Flash FAIL")
    return ok


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Build/flash STC-B Full Firmware v1")
    parser.add_argument("--flash", "-f", action="store_true")
    parser.add_argument("--flash-only", "-F", action="store_true")
    args = parser.parse_args()
    if args.flash_only:
        raise SystemExit(0 if flash() else 1)
    success = build()
    if success and args.flash:
        success = flash()
    raise SystemExit(0 if success else 1)
