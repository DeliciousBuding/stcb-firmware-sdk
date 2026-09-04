#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""常驻串口日志器（薄壳，线路基建在 serlink.py）
用法: python serial-log.py [COM口] [波特率] [日志文件路径]   # COM 缺省=env STC_PORT 或 COM3
行为: 每行时间戳追加写+即时 flush，直到 Ctrl+C 或端口死亡。
烧录走 tools/stcflash.py：它会自动杀掉本工具释放端口，烧完重开即可。"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serlink

serlink.utf8_console()
port = sys.argv[1] if len(sys.argv) > 1 else serlink.default_port()
baud = int(sys.argv[2]) if len(sys.argv) > 2 else serlink.DEFAULT_BAUD
logfile = sys.argv[3] if len(sys.argv) > 3 else None
emit = serlink.Emit(logfile)
sl = serlink.SerialLines(port, baud, on_line=emit)
print(f"[logger] {port}@{baud} -> {logfile or 'stdout'}", flush=True)
try:
    sl.open()
    while not sl.dead.is_set():
        time.sleep(0.2)
    print("[logger] 端口死亡，退出", flush=True)
except KeyboardInterrupt:
    pass
finally:
    sl.close()
    emit.close()
    print("[logger] stopped", flush=True)
