#!/usr/bin/env python3
"""Load larger UI faces first while contiguous heap is least fragmented."""
from pathlib import Path
import re
import sys

source = Path("src/SdCardFontSystem.cpp").read_text()
match = re.search(r"constexpr UiFontSize kUiFontSizes\[\] = \{(.*?)\};", source, re.S)
if not match:
    print("UI font size table missing", file=sys.stderr)
    raise SystemExit(1)
body = match.group(1)
pos12 = body.find("{UI_12_FONT_ID, 12}")
pos10 = body.find("{UI_10_FONT_ID, 10}")
pos8 = body.find("{SMALL_FONT_ID, 8}")
if min(pos12, pos10, pos8) < 0 or not (pos12 < pos10 < pos8):
    print("UI font sizes are not loaded largest-first", file=sys.stderr)
    raise SystemExit(1)
print("SD_UI_FONT_LARGEST_FIRST_OK")
