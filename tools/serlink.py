#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""serlink — 仓库级串口线路基建（serial-console / serial-log / debug-run / stcflash 共用）

调试链路第一性分层：
  编译(各 build.py) → 烧录(stcflash.py) → 观察/注入(本模块) → 断言(debug-run.py 场景层) → 证据(时间戳日志)。
本模块只管"一条串口线路"，四件事：
  - 端口约定：env STC_PORT > 默认 COM3（换口改环境变量，全工具链生效，勿改代码）
  - 行协议读取：后台线程按 \n 分行；ascii+replace 解码；NUL 清洗（原始串口垃圾曾把
    board-log 写成含 NUL，git 误判文本证据为二进制）；无 EOL 长行 [RAW-NOEOL] 兜底
  - 控制台安全：Windows GBK 控制台遇 U+FFFD 会打死 print（曾杀 RX 线程致 missed 误判
    FAIL）→ 入口统一调 utf8_console()
  - 端口释放：烧录前杀掉自家常驻串口工具（free_port；stcgal/pyserial 需独占端口）
API:
  sl = SerialLines(port=None, baud=9600, on_line=cb, on_dead=cb)
  sl.open(); sl.write(b"R"); sl.close()      # 可重复 open/close（powercycle 场景）
  Emit(path)                                  # 时间戳 stdout+证据文件双写 callable
"""
import datetime
import os
import subprocess
import sys
import threading

DEFAULT_BAUD = 9600


def default_port():
    """统一端口约定：env STC_PORT 优先，默认 COM3。"""
    return os.environ.get("STC_PORT", "COM3")


def utf8_console():
    """Windows 下把 stdout 重配 UTF-8+replace，防 GBK 控制台遇 U+FFFD 打死 print。"""
    if sys.platform == "win32":
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def ts():
    return datetime.datetime.now().strftime("%H:%M:%S")


def free_port(pattern="serial-console|serial-log|debug-run"):
    """杀掉占用 COM 口的自家串口工具（排除本进程）。"""
    me = os.getpid()
    ps = ('Get-CimInstance Win32_Process -Filter "Name like \'%python%\'" | '
          f'Where-Object {{ $_.CommandLine -match "{pattern}" }} | '
          'Select-Object -ExpandProperty ProcessId')
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-c", ps],
                             capture_output=True, text=True, timeout=15).stdout
        for pid in out.split():
            if pid.isdigit() and int(pid) != me:
                subprocess.run(["taskkill", "/PID", pid, "/F"], capture_output=True)
                print(f"[serlink] killed port holder pid {pid}")
    except Exception as e:
        print(f"[serlink] port cleanup warn: {e}")


class Emit:
    """时间戳 stdout + 可选证据文件双写（callable，直接作 SerialLines.on_line）。"""

    def __init__(self, path=None):
        self.path = path
        self.out = open(path, "a", encoding="utf-8", buffering=1) if path else None

    def __call__(self, text):
        rec = f"{ts()} {text}"
        print(rec, flush=True)
        if self.out:
            self.out.write(rec + "\n")

    def close(self):
        if self.out:
            self.out.close()


class SerialLines:
    """行协议读取器：后台线程读串口按行回调；open()/close() 可重复（重连场景）。"""

    def __init__(self, port=None, baud=DEFAULT_BAUD, on_line=None, on_dead=None):
        self.port = port or default_port()
        self.baud = baud
        self.on_line = on_line or (lambda t: print(t, flush=True))
        self.on_dead = on_dead
        self.dead = threading.Event()
        self.s = None

    def open(self):
        import serial   # 延迟导入：只烧录不串口的路径不需要 pyserial
        self.dead.clear()
        self.s = serial.Serial(self.port, self.baud, timeout=0.2)
        threading.Thread(target=self._rx, daemon=True).start()

    def _rx(self):
        buf = b""
        while not self.dead.is_set():
            try:
                d = self.s.read(256)
            except Exception:
                self.dead.set()
                if self.on_dead:
                    self.on_dead()
                return
            if not d:
                continue
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.on_line(line.decode("ascii", "replace").replace("\x00", "").strip())
            if len(buf) > 200:   # 无 EOL 垃圾兜底，防缓冲膨胀
                self.on_line("[RAW-NOEOL] " + buf.decode("ascii", "replace").replace("\x00", ""))
                buf = b""

    def write(self, data):
        self.s.write(data)

    def close(self):
        self.dead.set()
        try:
            if self.s:
                self.s.close()
        except Exception:
            pass
