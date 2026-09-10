#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
"""iap-probe 测试器：向板上 iap-probe 固件发 I 命令，收 IAP:RESULT:* 并断言。

用法:
  python run_test.py                # 跑一次自测并判定
  python run_test.py --dump         # 只发 S 重发上次结果，不重跑测试
  python run_test.py --evidence out.txt

判定（缺一不可，全部来自板端回执而非主机推测）：
  read_pre=blank / program=ok / read_post=match / erase=ok / read_after_erase=blank
cmdfail（越界地址是否被判非法）作为安全属性单独上报，不参与 PASS/FAIL——
它是加分项，失败不代表自写能力不成立。
"""
from __future__ import annotations

import argparse
import os
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import serlink  # 端口约定 + free_port + UTF-8 控制台

REQUIRED = {
    "read_pre": "blank",
    "program": "ok",
    "read_post": "match",
    "erase": "ok",
    "read_after_erase": "blank",
}


def parse_result(line: str) -> tuple[str, str] | None:
    """从 'IAP:RESULT:step=<name> state=<value> ...' 取出 (step, state)。"""
    if not line.startswith("IAP:RESULT:"):
        return None
    body = line[len("IAP:RESULT:"):]
    fields: dict[str, str] = {}
    for part in body.split():
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key] = value
    step = fields.get("step")
    state = fields.get("state")
    if step is None or state is None:
        return None
    return step, state


def main() -> int:
    serlink.utf8_console()
    parser = argparse.ArgumentParser(description="iap-probe self-test runner")
    parser.add_argument("--port", default=serlink.default_port())
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=15.0, help="等待 total= 的秒数")
    parser.add_argument("--dump", action="store_true", help="只发 S 重发上次结果")
    parser.add_argument("--evidence", type=pathlib.Path, default=None)
    args = parser.parse_args()

    import serial
    serlink.free_port()

    evidence = None
    if args.evidence:
        evidence = args.evidence.open("a", encoding="utf-8", buffering=1)

    def record(text: str) -> None:
        stamped = f"{time.strftime('%H:%M:%S')} {text}"
        print(stamped, flush=True)
        if evidence:
            evidence.write(stamped + "\n")

    results: dict[str, str] = {}
    raw_lines: list[str] = []
    total_state: str | None = None

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    try:
        time.sleep(0.4)
        ser.reset_input_buffer()

        command = b"S" if args.dump else b"I"
        record(f"[run] send {command.decode()} -> {args.port}@{args.baud}")
        ser.write(command)
        ser.flush()

        deadline = time.time() + args.timeout
        buffer = b""
        done = False
        while time.time() < deadline and not done:
            chunk = ser.read(256)
            if not chunk:
                continue
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                text = line.decode("ascii", "replace").replace("\x00", "").strip("\r")
                if not text:
                    continue
                record(text)
                raw_lines.append(text)
                if text.startswith("IAP:START"):
                    results.clear()
                parsed = parse_result(text)
                if parsed:
                    step, state = parsed
                    if step == "total":
                        total_state = state
                        done = True
                        break
                    results[step] = state
    finally:
        ser.close()

    try:
        if total_state is None:
            record("[FAIL] 未收到 IAP:RESULT:step=total —— 板上不是 iap-probe 固件？串口被占用？")
            return 1

        failures = [f"{step}: expected {want}, got {results.get(step, 'missing')}"
                    for step, want in REQUIRED.items()
                    if results.get(step) != want]
        if total_state != "PASS":
            failures.append(f"total: expected PASS, got {total_state}")

        cmdfail_line = next((l for l in raw_lines if "step=cmdfail" in l), "(missing)")
        record(f"[info] {cmdfail_line}")

        if failures:
            record("[FAIL] IAP 自写能力未证实：")
            for item in failures:
                record("       - " + item)
            return 1

        record("[PASS] IAP 自写能力已在真板证实（读/写/擦全链路 + 扇区擦除后全空白）")
        return 0
    finally:
        if evidence:
            evidence.close()


if __name__ == "__main__":
    raise SystemExit(main())
