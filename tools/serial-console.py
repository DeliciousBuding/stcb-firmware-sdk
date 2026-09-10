#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
"""交互式串口控制台（薄壳，线路基建在 serlink.py）
用法: python serial-console.py [COM口] [波特率] [日志文件路径]   # COM 缺省=env STC_PORT 或 COM3
行为: 收——板端输出全部时间戳追加到 stdout+日志文件；
      发——stdin 读行原样发字节到板子（如 O / R / S），回显 [TX]。
      Ctrl+C / quit 退出。烧录走 tools/stcflash.py（自动杀本进程释放端口），烧完重开即可。"""
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serlink

serlink.utf8_console()
port = sys.argv[1] if len(sys.argv) > 1 else serlink.default_port()
baud = int(sys.argv[2]) if len(sys.argv) > 2 else serlink.DEFAULT_BAUD
logfile = sys.argv[3] if len(sys.argv) > 3 else None
emit = serlink.Emit(logfile)
sl = serlink.SerialLines(port, baud, on_line=emit)


def tx_loop():
    for line in sys.stdin:
        cmd = line.strip()
        if not cmd:
            continue
        if cmd.lower() in ("quit", "exit"):
            break
        sl.write(cmd.encode("ascii", "ignore"))
        emit(f"[TX] {cmd}")


print(f"[console] {port}@{baud} -> {logfile or 'stdout'}; stdin=发送命令, Ctrl+C/quit=退出", flush=True)
try:
    sl.open()
    threading.Thread(target=tx_loop, daemon=True).start()
    while not sl.dead.is_set():
        time.sleep(0.2)
except KeyboardInterrupt:
    pass
finally:
    sl.close()
    emit.close()
    print("[console] stopped", flush=True)
