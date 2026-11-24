#!/usr/bin/env python3
"""
Noise Pollution Monitor — Laptop & Phone LAN Companion Dashboard Server

Serves the embedded responsive line-chart dashboard (main/web/index.html) over
your local network so both your laptop and phone can view live noise telemetry:

  1. Demo mode (no hardware or extra packages needed):
       python3 dashboard/bridge_server.py --demo

  2. USB UART bridge mode (streams live ESP32-S3 serial output to laptop + phone):
       pip install pyserial
       python3 dashboard/bridge_server.py --port /dev/cu.usbmodem14101 --baud 115200
"""

from __future__ import annotations

import argparse
import collections
import http.server
import json
import math
import os
import random
import re
import socket
import socketserver
import sys
import threading
import time
from pathlib import Path
from typing import Any, Deque, Dict, Optional

PRESETS = [
    {"name": "DEFAULT", "trigger": 65.0, "reset": 60.0},
    {"name": "QUIET/LIB", "trigger": 45.0, "reset": 40.0},
    {"name": "RESIDENT", "trigger": 65.0, "reset": 60.0},
    {"name": "INDUSTRIAL", "trigger": 80.0, "reset": 74.0},
]

TEXT_LOG_RE = re.compile(
    r"\|\s*(Noise|Level):\s*([\d.]+)\s*(dB|%)\s*(?:\(REL\))?\s*"
    r"\|\s*RMS:\s*([\d.]+)\s*(\w+)\s*"
    r"\|\s*Thr:\s*([\d.]+)\s*\(([^)]+)\)\s*"
    r"\|\s*Status:\s*(\w+)\s*"
    r"\|\s*Lat:\s*([\d.]+)"
)


def detect_lan_ip() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


class TelemetryStore:
    def __init__(self, source_label: str, lan_ip: str, capacity: int = 240) -> None:
        self.lock = threading.Lock()
        self.source_label = source_label
        self.lan_ip = lan_ip
        self.history: Deque[list] = collections.deque(maxlen=capacity)
        self.trigger = 65.0
        self.reset = 60.0
        self.preset_index = 0
        self.muted = False
        self.alert_latched = False
        self.latest: Dict[str, Any] = {
            "seq": 0,
            "uptime_ms": 0,
            "primary_value": 0.0,
            "relative_pct": 0.0,
            "estimated_db": 20.0,
            "rms_raw": 0.0,
            "rms_smoothed": 0.0,
            "dc_offset": 1650.0,
            "threshold_trigger": self.trigger,
            "threshold_reset": self.reset,
            "is_calibrated": False,
            "is_millivolts": True,
            "alert_active": False,
            "muted": False,
            "status": "NORMAL",
            "preset_name": "DEFAULT",
            "latency_us": 0,
            "ip": lan_ip,
            "source": source_label,
        }

    def record(self, sample: Dict[str, Any]) -> None:
        with self.lock:
            self.latest = sample
            status = sample.get("status", "NORMAL")
            st_code = 1 if status == "WARNING" else (2 if status == "MUTED" else 0)
            self.history.append([
                int(sample.get("uptime_ms", 0)),
                round(float(sample.get("primary_value", 0.0)), 1),
                round(float(sample.get("rms_smoothed", 0.0)), 1),
                round(float(sample.get("threshold_trigger", self.trigger)), 1),
                st_code,
            ])

    def snapshot_live(self) -> Dict[str, Any]:
        with self.lock:
            return dict(self.latest)

    def snapshot_history(self) -> Dict[str, Any]:
        with self.lock:
            return {
                "is_calibrated": bool(self.latest.get("is_calibrated", False)),
                "is_millivolts": bool(self.latest.get("is_millivolts", True)),
                "threshold_reset": float(self.latest.get("threshold_reset", self.reset)),
                "preset_name": str(self.latest.get("preset_name", "DEFAULT")),
                "points": list(self.history),
            }

    def apply_control(self, cmd: Dict[str, Any]) -> bool:
        with self.lock:
            changed = False
            if isinstance(cmd.get("mute"), bool):
                self.muted = cmd["mute"]
                changed = True
            if isinstance(cmd.get("preset"), int):
                idx = cmd["preset"]
                if 0 <= idx < len(PRESETS):
                    self.preset_index = idx
                    self.trigger = PRESETS[idx]["trigger"]
                    self.reset = PRESETS[idx]["reset"]
                    changed = True
            if isinstance(cmd.get("trigger"), (int, float)) and isinstance(cmd.get("reset"), (int, float)):
                trig = float(cmd["trigger"])
                rst = float(cmd["reset"])
                if 10.0 <= trig <= 125.0 and 5.0 <= rst < trig:
                    self.preset_index = len(PRESETS)
                    self.trigger = trig
                    self.reset = rst
                    changed = True
            return changed


def parse_uart_line(line: str, store: TelemetryStore, start_time: float) -> Optional[Dict[str, Any]]:
    s = line.strip()
    if not s or s.startswith("uptime_ms,"):
        return None

    # CSV format: uptime_ms,seq,rms_raw,rms_smooth,relative_pct,est_db,calibrated,threshold_trigger,status,latency_us
    if re.match(r"^\d+,\d+,", s):
        parts = s.split(",")
        if len(parts) >= 10:
            try:
                uptime_ms = int(parts[0])
                seq = int(parts[1])
                rms_raw = float(parts[2])
                rms_smooth = float(parts[3])
                rel_pct = float(parts[4])
                est_db = float(parts[5])
                is_cal = parts[6].strip() == "1"
                trig = float(parts[7])
                status = parts[8].strip()
                lat_us = int(parts[9])
                return {
                    "seq": seq,
                    "uptime_ms": uptime_ms,
                    "primary_value": est_db if is_cal else rel_pct,
                    "relative_pct": rel_pct,
                    "estimated_db": est_db,
                    "rms_raw": rms_raw,
                    "rms_smoothed": rms_smooth,
                    "dc_offset": 1650.0,
                    "threshold_trigger": trig,
                    "threshold_reset": max(0.0, trig - 5.0),
                    "is_calibrated": is_cal,
                    "is_millivolts": True,
                    "alert_active": status in ("WARNING", "MUTED"),
                    "muted": status == "MUTED",
                    "status": status,
                    "preset_name": "UART",
                    "latency_us": lat_us,
                    "ip": store.lan_ip,
                    "source": store.source_label,
                }
            except ValueError:
                return None

    # Human-readable format from logger.c
    m = TEXT_LOG_RE.search(s)
    if m:
        is_cal = m.group(1) == "Noise"
        val = float(m.group(2))
        rms = float(m.group(4))
        unit = m.group(5)
        trig = float(m.group(6))
        preset = m.group(7)
        status = m.group(8)
        lat_ms = float(m.group(9))
        return {
            "seq": int(store.snapshot_live().get("seq", 0)) + 1,
            "uptime_ms": int((time.monotonic() - start_time) * 1000),
            "primary_value": val,
            "relative_pct": val if not is_cal else 0.0,
            "estimated_db": val if is_cal else 20.0,
            "rms_raw": rms,
            "rms_smoothed": rms,
            "dc_offset": 1650.0,
            "threshold_trigger": trig,
            "threshold_reset": max(0.0, trig - 5.0),
            "is_calibrated": is_cal,
            "is_millivolts": unit == "mV",
            "alert_active": status in ("WARNING", "MUTED"),
            "muted": status == "MUTED",
            "status": status,
            "preset_name": preset,
            "latency_us": int(lat_ms * 1000),
            "ip": store.lan_ip,
            "source": store.source_label,
        }

    return None


def demo_producer(store: TelemetryStore, stop_event: threading.Event) -> None:
    start = time.monotonic()
    seq = 0
    phase = 0.0
    rms_ema = 48.0
    spike_ticks = 0

    while not stop_event.is_set():
        seq += 1
        phase += 0.17
        if spike_ticks > 0:
            spike_ticks -= 1
        elif random.random() < 0.05:
            spike_ticks = random.randint(8, 18)

        base_rms = 45.0 + 18.0 * math.sin(phase) + 9.0 * math.sin(phase * 2.4) + random.uniform(-8.0, 8.0)
        spike_rms = random.uniform(120.0, 260.0) if spike_ticks > 0 else 0.0
        rms_raw = max(4.0, base_rms + spike_rms)
        rms_ema = 0.25 * rms_raw + 0.75 * rms_ema

        ratio = max(0.01, min(1.0, rms_ema / 450.0))
        rel_pct = ((20.0 * math.log10(ratio) + 40.0) / 40.0) * 100.0
        est_db = max(20.0, min(130.0, 94.0 + 20.0 * math.log10(max(0.1, rms_ema) / 500.0)))

        with store.lock:
            trig = store.trigger
            rst = store.reset
            if not store.alert_latched:
                if rel_pct >= trig:
                    store.alert_latched = True
            else:
                if rel_pct <= rst:
                    store.alert_latched = False
                    store.muted = False
            alert_active = store.alert_latched
            muted = store.muted
            preset_name = PRESETS[store.preset_index]["name"] if store.preset_index < len(PRESETS) else "CUSTOM"

        status = "MUTED" if (alert_active and muted) else ("WARNING" if alert_active else "NORMAL")
        store.record({
            "seq": seq,
            "uptime_ms": int((time.monotonic() - start) * 1000),
            "primary_value": round(rel_pct, 2),
            "relative_pct": round(rel_pct, 2),
            "estimated_db": round(est_db, 2),
            "rms_raw": round(rms_raw, 2),
            "rms_smoothed": round(rms_ema, 2),
            "dc_offset": 1650.0,
            "threshold_trigger": trig,
            "threshold_reset": rst,
            "is_calibrated": False,
            "is_millivolts": True,
            "alert_active": alert_active,
            "muted": muted,
            "status": status,
            "preset_name": preset_name,
            "latency_us": random.randint(380, 690),
            "ip": store.lan_ip,
            "source": store.source_label,
        })
        stop_event.wait(0.25)


def serial_producer(port: str, baud: int, store: TelemetryStore, stop_event: threading.Event) -> None:
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial is required for --port mode. Install with: pip install pyserial", file=sys.stderr)
        os._exit(1)

    start = time.monotonic()
    while not stop_event.is_set():
        try:
            print(f"[UART] Opening {port} @ {baud} baud...")
            with serial.Serial(port, baud, timeout=1.0) as ser:
                print(f"[UART] Connected to {port}. Streaming live data to dashboard.")
                while not stop_event.is_set():
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="replace")
                    parsed = parse_uart_line(line, store, start)
                    if parsed is not None:
                        store.record(parsed)
        except Exception as exc:  # pragma: no cover
            print(f"[UART] Reconnecting after error: {exc}", file=sys.stderr)
            stop_event.wait(2.0)


def make_handler(html_bytes: bytes, store: TelemetryStore):
    class DashboardHandler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args: Any) -> None:
            # Keep terminal output concise
            return

        def _send_headers(self, code: int, content_type: str, length: int) -> None:
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(length))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()

        def do_OPTIONS(self) -> None:
            self._send_headers(204, "text/plain", 0)

        def do_GET(self) -> None:
            path = self.path.split("?", 1)[0]
            if path in ("/", "/index.html"):
                self._send_headers(200, "text/html; charset=utf-8", len(html_bytes))
                self.wfile.write(html_bytes)
            elif path == "/api/live":
                body = json.dumps(store.snapshot_live()).encode("utf-8")
                self._send_headers(200, "application/json", len(body))
                self.wfile.write(body)
            elif path == "/api/history":
                body = json.dumps(store.snapshot_history()).encode("utf-8")
                self._send_headers(200, "application/json", len(body))
                self.wfile.write(body)
            else:
                body = b'{"error":"not found"}'
                self._send_headers(404, "application/json", len(body))
                self.wfile.write(body)

        def do_POST(self) -> None:
            path = self.path.split("?", 1)[0]
            if path != "/api/control":
                body = b'{"error":"not found"}'
                self._send_headers(404, "application/json", len(body))
                self.wfile.write(body)
                return

            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(min(length, 1024))
            try:
                cmd = json.loads(raw.decode("utf-8"))
            except json.JSONDecodeError:
                body = b'{"ok":false,"error":"invalid json"}'
                self._send_headers(400, "application/json", len(body))
                self.wfile.write(body)
                return

            ok = store.apply_control(cmd)
            body = json.dumps({"ok": ok}).encode("utf-8")
            self._send_headers(200 if ok else 400, "application/json", len(body))
            self.wfile.write(body)

    return DashboardHandler


class ReusableTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> None:
    parser = argparse.ArgumentParser(description="Noise Monitor Laptop/Phone Dashboard Server")
    parser.add_argument("--port", help="ESP32-S3 serial port (e.g. /dev/cu.usbmodem14101 or COM5)")
    parser.add_argument("--baud", type=int, default=115200, help="UART baud rate (default: 115200)")
    parser.add_argument("--demo", action="store_true", help="Run built-in live noise simulator")
    parser.add_argument("--http-port", type=int, default=8080, help="HTTP server port (default: 8080)")
    parser.add_argument("--host", default="0.0.0.0", help="Bind address (default: 0.0.0.0 for phone + laptop access)")
    args = parser.parse_args()

    if not args.port and not args.demo:
        args.demo = True

    html_path = Path(__file__).resolve().parent.parent / "main" / "web" / "index.html"
    if not html_path.is_file():
        print(f"ERROR: Dashboard HTML not found at {html_path}", file=sys.stderr)
        sys.exit(1)

    html_bytes = html_path.read_bytes()
    lan_ip = detect_lan_ip()
    source_label = "LAN Demo Bridge" if args.demo else f"USB Bridge ({args.port})"
    store = TelemetryStore(source_label=source_label, lan_ip=lan_ip)

    stop_event = threading.Event()
    if args.demo:
        worker = threading.Thread(target=demo_producer, args=(store, stop_event), daemon=True)
    else:
        worker = threading.Thread(target=serial_producer, args=(args.port, args.baud, store, stop_event), daemon=True)
    worker.start()

    handler_cls = make_handler(html_bytes, store)
    with ReusableTCPServer((args.host, args.http_port), handler_cls) as httpd:
        print("=" * 60)
        print(" Real-Time Noise Pollution Monitor — Live Web Dashboard")
        print("=" * 60)
        print(f" Source : {source_label}")
        print(f" Laptop : http://localhost:{args.http_port}")
        print(f" Phone  : http://{lan_ip}:{args.http_port}  (same Wi-Fi network)")
        print(" Press Ctrl+C to stop.")
        print("=" * 60)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down dashboard server...")
        finally:
            stop_event.set()


if __name__ == "__main__":
    main()
