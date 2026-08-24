#!/usr/bin/env python3
"""Real-device font-download test driver for CrossPoint Reader (Xteink X4).

Drives the device over its USB-CDC serial console:
  - CMD:INPUT:<BUTTON>:TAP        inject a button tap (ENABLE_SERIAL_INPUT_TEST)
  - CMD:SCREENSHOT                dump the 800x480 1bpp framebuffer

The SCREENSHOT reply is "SCREENSHOT_START:<size>\\n" + <size> raw framebuffer
bytes + "SCREENSHOT_END\\n".  Framebuffer bytes are NOT newline-safe, so the
reader works on the raw byte stream (never splits on \\n while a screenshot is
in flight).

Usage:
  python device_font_download_driver.py <port> <command>
Commands:
  interactive            REPL: "tap BACK", "shot", "cmd CMD:...", "grep STR", "logs"
  shot                   take one screenshot to screenshots/shot_<n>.png
  monitor N              listen for N seconds, printing log lines
"""
from __future__ import annotations

import argparse
import os
import threading
import time

import serial
from PIL import Image

WIDTH, HEIGHT = 800, 480  # raw framebuffer is landscape; rotate 270 for portrait
MARKER = b"SCREENSHOT_START:"


class Device:
    def __init__(self, port: str, baud: int = 115200):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.lines: list[str] = []
        self._lock = threading.Lock()
        self._cond = threading.Condition()
        self._stop = threading.Event()
        self._screenshot_seen = threading.Event()
        self.shot_count = 0
        self._expecting = False
        self._shot_size = 0
        self._shot_data = b""
        self._buf = b""
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    # -- low-level -----------------------------------------------------------
    def _ingest_lines(self, chunk: bytes):
        if not chunk:
            return
        for raw in chunk.split(b"\n"):
            text = raw.decode("utf-8", errors="replace").rstrip("\r")
            if not text:
                continue
            with self._lock:
                self.lines.append(text)
                if len(self.lines) > 20000:
                    self.lines = self.lines[-10000:]
        with self._cond:
            self._cond.notify_all()

    def _finish_shot(self):
        self._expecting = False
        self._save_shot()
        self._screenshot_seen.set()

    def _read_loop(self):
        while not self._stop.is_set():
            data = self.ser.read(4096)
            if not data:
                continue
            if self._expecting:
                need = self._shot_size - len(self._shot_data)
                take = min(need, len(data))
                self._shot_data += data[:take]
                if len(self._shot_data) >= self._shot_size:
                    self._finish_shot()
                leftover = data[take:]
                if leftover:
                    self._buf += leftover
                continue
            self._buf += data
            idx = self._buf.find(MARKER)
            if idx >= 0:
                self._ingest_lines(self._buf[:idx])
                tail = self._buf[idx + len(MARKER):]
                nl = tail.find(b"\n")
                if nl < 0:
                    # size line not complete yet; wait for more data
                    self._buf = self._buf[idx:]
                    continue
                size_text = tail[:nl].strip()
                try:
                    self._shot_size = int(size_text.decode())
                except ValueError:
                    self._buf = b""
                    continue
                self._shot_data = b""
                self._expecting = True
                extra = tail[nl + 1:]
                if extra:
                    take = min(self._shot_size, len(extra))
                    self._shot_data = extra[:take]
                    self._buf = extra[take:]
                    if len(self._shot_data) >= self._shot_size:
                        self._finish_shot()
                else:
                    self._buf = b""
                continue
            # no marker yet: ingest complete lines, keep trailing partial
            nl = self._buf.rfind(b"\n")
            if nl >= 0:
                self._ingest_lines(self._buf[: nl + 1])
                self._buf = self._buf[nl + 1:]
            elif len(self._buf) > len(MARKER):
                # keep only the tail in case MARKER spans a read boundary
                self._buf = self._buf[-(len(MARKER) - 1):]

    def _save_shot(self):
        os.makedirs("screenshots", exist_ok=True)
        expected = (WIDTH // 8) * HEIGHT
        data = self._shot_data[:expected]
        if len(data) != expected:
            print(f"[shot] WARN: buffer {len(data)} != expected {expected}", flush=True)
        # 1bpp: 0 = black, 1 = white. Keep raw; also write inverted variant.
        img = Image.frombytes("1", (WIDTH, HEIGHT), data)
        img = img.transpose(Image.ROTATE_270)
        self.shot_count += 1
        base = f"screenshots/shot_{self.shot_count:03d}"
        img.save(base + ".png")
        Image.eval(img, lambda p: 1 - p).save(base + "_inv.png")
        print(f"[shot] saved {base}.png ({len(data)} bytes)", flush=True)

    def send(self, text: str):
        self.ser.write(text.encode())
        time.sleep(0.05)

    def cmd(self, cmd: str, wait: float = 0.5):
        print(f"[send] CMD:{cmd}", flush=True)
        self.send(f"CMD:{cmd}\n")
        if wait:
            time.sleep(wait)

    def tap(self, button: str, wait: float = 0.6):
        self.cmd(f"INPUT:{button}:TAP", wait)

    def shot(self) -> str:
        self._screenshot_seen.clear()
        self.cmd("SCREENSHOT", 0.2)
        ok = self._screenshot_seen.wait(timeout=30.0)
        if not ok:
            print("[shot] TIMEOUT waiting for framebuffer", flush=True)
        time.sleep(0.3)
        return f"screenshots/shot_{self.shot_count:03d}.png"

    def loggrep(self, needle: str, tail: int = 20000, timeout: float = 3.0):
        deadline = time.time() + timeout
        out = []
        while time.time() < deadline:
            with self._lock:
                out = [l for l in self.lines[-tail:] if needle in l]
            if out:
                break
            time.sleep(0.2)
        return out

    def drain(self, seconds: float = 1.0):
        time.sleep(seconds)

    def close(self):
        self._stop.set()
        self.ser.close()


def print_logs(dev: Device, needle: str = "", tail: int = 150):
    with dev._lock:
        lines = dev.lines[-tail:]
    for l in lines:
        if not needle or needle in l:
            print(l)


def do_interactive(dev: Device):
    print("Interactive. Commands: tap BUTTON | shot | cmd CMD | grep STR | logs | clear | quit")
    while True:
        try:
            raw = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not raw:
            continue
        parts = raw.split(" ", 1)
        verb = parts[0].upper()
        arg = parts[1] if len(parts) > 1 else ""
        if verb == "TAP":
            dev.tap(arg)
        elif verb == "SHOT":
            dev.shot()
        elif verb == "CMD":
            dev.cmd(arg, 0.8)
        elif verb in ("GREP", "G"):
            for l in dev.loggrep(arg):
                print(l)
        elif verb == "LOGS":
            print_logs(dev)
        elif verb == "CLEAR":
            with dev._lock:
                dev.lines.clear()
            print("cleared")
        elif verb == "QUIT":
            break
        else:
            print("unknown verb:", verb)


def do_shot(dev: Device):
    dev.drain(0.5)
    p = dev.shot()
    print(p)


def do_monitor(dev: Device, seconds: float, needle: str):
    deadline = time.time() + seconds
    last = len(dev.lines)
    while time.time() < deadline:
        with dev._cond:
            dev._cond.wait(timeout=0.5)
        with dev._lock:
            new = dev.lines[last:]
            last = len(dev.lines)
        for l in new:
            if not needle or needle in l:
                print(l)
    print("[monitor] done")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("command", nargs="?", default="interactive")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--grep", default="")
    args = ap.parse_args()

    dev = Device(args.port, args.baud)
    try:
        cmd = args.command.lower()
        if cmd == "interactive":
            do_interactive(dev)
        elif cmd == "shot":
            do_shot(dev)
        elif cmd == "monitor":
            do_monitor(dev, args.seconds, args.grep)
        elif cmd == "logs":
            print_logs(dev, args.grep)
        else:
            print(f"unknown command {cmd}")
    finally:
        dev.close()


if __name__ == "__main__":
    main()
