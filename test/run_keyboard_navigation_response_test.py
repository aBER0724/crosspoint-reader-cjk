#!/usr/bin/env python3
"""Keyboard button navigation must repaint without blocking light-mode input."""
from pathlib import Path
import re
import sys

header = Path("src/activities/util/KeyboardEntryActivity.h").read_text()
source = Path("src/activities/util/KeyboardEntryActivity.cpp").read_text()

navigator = re.search(r"ButtonNavigator\s+buttonNavigator(?:\{([^}]*)\})?;", header)
if navigator is None or navigator.group(1) is None:
    print("keyboard uses the slow global ButtonNavigator repeat defaults", file=sys.stderr)
    raise SystemExit(1)

values = [int(value.strip()) for value in navigator.group(1).split(",")]
if len(values) != 2 or values[0] > 175 or values[1] > 300:
    print("keyboard repeat interval/start are still too slow", file=sys.stderr)
    raise SystemExit(1)

render_start = source.find("void KeyboardEntryActivity::render")
render_end = source.find("void KeyboardEntryActivity::onComplete", render_start)
if render_start < 0 or render_end < 0:
    print("keyboard render body not found", file=sys.stderr)
    raise SystemExit(1)
render = source[render_start:render_end]

if "renderer.isDarkMode()" not in render or "renderer.displayBufferDarkRedrive()" not in render:
    print("keyboard dark-mode repaint no longer preserves DarkRedrive", file=sys.stderr)
    raise SystemExit(1)
if "renderer.displayBufferAsync()" not in render:
    print("keyboard light-mode repaint still blocks button polling", file=sys.stderr)
    raise SystemExit(1)

print("KEYBOARD_NAVIGATION_RESPONSE_OK")
