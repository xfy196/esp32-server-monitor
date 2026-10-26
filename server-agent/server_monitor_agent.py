#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
server_monitor_agent.py
=======================

给 ESP32 OLED 监控屏提供极简 JSON 指标接口。

特点
----
* 只依赖 Python 3 标准库（读 /proc、/sys、statvfs），无需 pip install
* 后台线程每 1 秒采样一次，HTTP 请求永远立即返回
* 可选 Token 鉴权，默认只建议在局域网内使用
* --demo 模式可以生成假数据，用于在没有 Linux 服务器时调试屏幕

用法
----
    python3 server_monitor_agent.py --port 8787 --token 你的密钥
    python3 server_monitor_agent.py --demo --port 8787      # 假数据
    python3 server_monitor_agent.py --once --demo           # 打印一次就退出

接口
----
    GET /metrics   -> 指标 JSON（ESP32 用这个）
    GET /health    -> 存活检查
"""

import argparse
import hmac
import json
import math
import os
import platform
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

VERSION = "1.0.0"
PROC = "/proc"
STARTED_AT = time.time()

# 由 main() 填充
sampler = None
TOKEN = ""
QUIET = False


# --------------------------------------------------------------------------
# 读取系统指标（Linux）
# --------------------------------------------------------------------------
def _read(path, default=""):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return default


def _cpu_times():
    """返回 (idle+iowait, total) 累计值；非 Linux 返回 None。"""
    head = _read(PROC + "/stat").split("\n", 1)[0].split()
    if len(head) < 5 or head[0] != "cpu":
        return None
    try:
        vals = [int(x) for x in head[1:9]]
    except ValueError:
        return None
    while len(vals) < 8:
        vals.append(0)
    idle = vals[3] + vals[4]          # idle + iowait
    return idle, sum(vals)


def _kb_block(total_kb, used_kb):
    return {
        "total_kb": total_kb,
        "used_kb": used_kb,
        "percent": round(100.0 * used_kb / total_kb, 1) if total_kb else 0.0,
    }


def _meminfo():
    info = {}
    for line in _read(PROC + "/meminfo").splitlines():
        key, _, rest = line.partition(":")
        parts = rest.split()
        if parts:
            try:
                info[key.strip()] = int(parts[0])
            except ValueError:
                pass
    if "MemTotal" not in info:
        return None, None

    total = info["MemTotal"]
    avail = info.get("MemAvailable", info.get("MemFree", 0))
    mem = _kb_block(total, max(total - avail, 0))

    swap_total = info.get("SwapTotal", 0)
    swap_free = info.get("SwapFree", 0)
    swap = _kb_block(swap_total, max(swap_total - swap_free, 0))
    return mem, swap


def _disk(path="/"):
    try:
        st = os.statvfs(path)
    except OSError:
        return None
    total = st.f_blocks * st.f_frsize
    free = st.f_bfree * st.f_frsize
    avail = st.f_bavail * st.f_frsize
    used = max(total - free, 0)
    denom = used + avail            # 与 df 的 Use% 一致（排除 root 预留）
    return {
        "total_kb": total // 1024,
        "used_kb": used // 1024,
        "percent": round(100.0 * used / denom, 1) if denom else 0.0,
    }


def _net_bytes():
    """所有非 lo 网卡的累计收发字节数。"""
    rx = tx = 0
    for line in _read(PROC + "/net/dev").splitlines():
        if ":" not in line:
            continue
        name, rest = line.split(":", 1)
        if name.strip() == "lo":
            continue
        cols = rest.split()
        if len(cols) < 9:
            continue
        try:
            rx += int(cols[0])
            tx += int(cols[8])
        except ValueError:
            pass
    return rx, tx


def _temperature():
    """从 thermal zone 读温度，优先 CPU / SoC 类型。"""
    base = "/sys/class/thermal"
    try:
        zones = sorted(z for z in os.listdir(base) if z.startswith("thermal_zone"))
    except OSError:
        return None

    fallback = None
    for zone in zones:
        raw = _read(os.path.join(base, zone, "temp")).strip()
        try:
            value = int(raw) / 1000.0
        except ValueError:
            continue
        if not 0 < value < 150:
            continue
        ztype = _read(os.path.join(base, zone, "type")).strip().lower()
        if any(k in ztype for k in ("x86_pkg", "coretemp", "cpu", "soc", "package")):
            return round(value, 1)
        if fallback is None:
            fallback = round(value, 1)
    return fallback


def _uptime():
    parts = _read(PROC + "/uptime").split()
    try:
        return int(float(parts[0]))
    except (IndexError, ValueError):
        return 0


# --------------------------------------------------------------------------
# 采样线程
# --------------------------------------------------------------------------
class Sampler(threading.Thread):
    def __init__(self, interval=1.0, demo=False):
        super().__init__(name="sampler", daemon=True)
        self.interval = max(0.2, interval)
        self.demo = demo
        self._lock = threading.Lock()
        self._snap = {
            "agent": VERSION,
            "ts": int(time.time()),
            "host": socket.gethostname()[:31],
            "cpu": 0.0,
        }
        self._prev_cpu = None if demo else _cpu_times()
        self._t0 = time.time()

    def snapshot(self):
        with self._lock:
            return dict(self._snap)

    def run(self):
        while True:
            try:
                snap = self._demo_snapshot() if self.demo else self._real_snapshot()
                with self._lock:
                    self._snap = snap
            except Exception as exc:  # 采样失败不能拖垮 HTTP 服务
                print("[sampler] %s" % exc, file=sys.stderr)
            time.sleep(self.interval)

    # -- 真实数据 ---------------------------------------------------------
    def _real_snapshot(self):
        cpu = 0.0
        cur = _cpu_times()
        if cur and self._prev_cpu:
            d_idle = cur[0] - self._prev_cpu[0]
            d_total = cur[1] - self._prev_cpu[1]
            if d_total > 0:
                cpu = 100.0 * (1.0 - float(d_idle) / d_total)
        self._prev_cpu = cur or self._prev_cpu

        mem, swap = _meminfo()
        rx, tx = _net_bytes()
        try:
            loads = [round(x, 2) for x in os.getloadavg()]
        except OSError:
            loads = [0.0, 0.0, 0.0]

        return {
            "agent": VERSION,
            "ts": int(time.time()),
            "host": socket.gethostname()[:31],
            "kernel": platform.release(),
            "uptime": _uptime(),
            "cores": os.cpu_count() or 1,
            "load": loads,
            "cpu": round(max(0.0, min(100.0, cpu)), 1),
            "mem": mem,
            "swap": swap,
            "disk": _disk("/"),
            "net": {"rx_bytes": rx, "tx_bytes": tx},
            "temp_c": _temperature(),
        }

    # -- 演示数据 ---------------------------------------------------------
    def _demo_snapshot(self):
        t = time.time() - self._t0
        cpu = 38 + 22 * math.sin(t / 9.0) + 9 * math.sin(t / 2.3)
        mem = 46 + 12 * math.sin(t / 27.0 + 1.0)
        disk = 63 + 2 * math.sin(t / 90.0)

        mem_total = 8 * 1024 * 1024                      # kB
        disk_total = 32 * 1024 * 1024                    # kB
        return {
            "agent": VERSION,
            "ts": int(time.time()),
            "host": "demo-server",
            "kernel": "6.8.0-demo",
            "uptime": int(3 * 86400 + 4 * 3600 + t),
            "cores": 4,
            "load": [
                round(0.4 + 0.3 * math.sin(t / 11.0), 2),
                round(0.5 + 0.2 * math.sin(t / 17.0), 2),
                round(0.6 + 0.1 * math.sin(t / 23.0), 2),
            ],
            "cpu": round(max(1.0, min(99.0, cpu)), 1),
            "mem": _kb_block(mem_total, int(mem_total * mem / 100)),
            "swap": _kb_block(2 * 1024 * 1024, 0),
            "disk": {
                "total_kb": disk_total,
                "used_kb": int(disk_total * disk / 100),
                "percent": round(disk, 1),
            },
            "net": {
                "rx_bytes": int(2.4e6 * t + 6e5 * math.sin(t / 4.0)),
                "tx_bytes": int(9e5 * t + 2e5 * math.sin(t / 6.0)),
            },
            "temp_c": round(48 + 6 * math.sin(t / 23.0), 1),
        }


# --------------------------------------------------------------------------
# HTTP 服务
# --------------------------------------------------------------------------
class MonitorHandler(BaseHTTPRequestHandler):
    server_version = "ServerMonitorAgent/" + VERSION
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"

        if not self._authorized(parsed):
            self._send({"error": "unauthorized", "hint": "send header X-Token"}, 401)
            return

        if path in ("/", "/metrics", "/api/metrics"):
            self._send(sampler.snapshot(), 200)
        elif path == "/health":
            self._send(
                {"ok": True, "agent": VERSION, "uptime_s": int(time.time() - STARTED_AT)},
                200,
            )
        else:
            self._send({"error": "not found", "endpoints": ["/metrics", "/health"]}, 404)

    def _authorized(self, parsed):
        if not TOKEN:
            return True
        given = self.headers.get("X-Token") or self.headers.get("Authorization", "")
        if given.lower().startswith("bearer "):
            given = given[7:].strip()
        if not given:
            values = parse_qs(parsed.query).get("token") or [""]
            given = values[0]
        return hmac.compare_digest(given, TOKEN)

    def _send(self, payload, code=200):
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        try:
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, fmt, *args):
        if not QUIET:
            sys.stderr.write(
                "%s  %s\n" % (time.strftime("%H:%M:%S"), fmt % args)
            )


# --------------------------------------------------------------------------
def main():
    global sampler, TOKEN, QUIET

    parser = argparse.ArgumentParser(
        description="ESP32 OLED 服务器监控屏的数据接口",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--bind", default="0.0.0.0", help="监听地址")
    parser.add_argument("--port", type=int, default=8787, help="监听端口")
    parser.add_argument("--token", default=os.environ.get("MONITOR_TOKEN", ""),
                        help="访问密钥（也可用环境变量 MONITOR_TOKEN）")
    parser.add_argument("--interval", type=float, default=1.0, help="采样间隔（秒）")
    parser.add_argument("--demo", action="store_true", help="使用假数据，便于调试屏幕")
    parser.add_argument("--once", action="store_true", help="打印一次 JSON 后退出")
    parser.add_argument("--quiet", action="store_true", help="不打印访问日志")
    args = parser.parse_args()

    TOKEN = args.token
    QUIET = args.quiet
    sampler = Sampler(args.interval, demo=args.demo)

    if not args.demo and _cpu_times() is None:
        print("错误：未找到 %s/stat，本程序需要在 Linux 上采集真实数据。\n"
              "     在 macOS / Windows 上请加 --demo 预览。" % PROC, file=sys.stderr)
        return 2

    if args.once:
        snap = sampler._demo_snapshot() if args.demo else sampler._real_snapshot()
        print(json.dumps(snap, indent=2, ensure_ascii=False))
        return 0

    sampler.start()
    time.sleep(min(1.0, sampler.interval))     # 等首次采样完成

    try:
        httpd = ThreadingHTTPServer((args.bind, args.port), MonitorHandler)
    except OSError as exc:
        print("无法监听 %s:%s -> %s" % (args.bind, args.port, exc), file=sys.stderr)
        return 1
    httpd.daemon_threads = True

    print("")
    print("  Server Monitor Agent v%s" % VERSION)
    print("  ------------------------------------------")
    print("  监听地址 : http://%s:%d/metrics" % (args.bind, args.port))
    print("  采集模式 : %s" % ("DEMO（假数据）" if args.demo else "Linux /proc"))
    print("  鉴权     : %s" % ("Token 已启用" if TOKEN else "无（建议加上 --token）"))
    print("  测试     : curl%s http://127.0.0.1:%d/metrics" % (
        (" -H 'X-Token: %s'" % TOKEN) if TOKEN else "", args.port))
    print("")

    if not TOKEN:
        print("  提示：未设置 token，局域网内任何人都能读取指标。", file=sys.stderr)
        print("       正式使用请加 --token 并在固件 secrets.h 里填同一个值。\n",
              file=sys.stderr)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止。")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
