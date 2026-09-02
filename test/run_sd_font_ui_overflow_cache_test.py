#!/usr/bin/env python3
"""Guard the SD bitmap overflow cache against UI-page thrashing."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "lib" / "EpdFont" / "SdCardFont.h"
header = HEADER_PATH.read_text(encoding="utf-8")

match = re.search(r"OVERFLOW_CAPACITY\s*=\s*(\d+)", header)
if not match:
    print("missing OVERFLOW_CAPACITY", file=sys.stderr)
    raise SystemExit(1)

capacity = int(match.group(1))
if capacity < 80:
    print(f"overflow cache too small for a UI page: {capacity} < 80", file=sys.stderr)
    raise SystemExit(1)
if capacity > 80:
    print(f"overflow cache exceeds the low-memory budget: {capacity} > 80", file=sys.stderr)
    raise SystemExit(1)
if "OVERFLOW_BITMAP_BUDGET_BYTES" not in header:
    print("missing bounded overflow bitmap budget", file=sys.stderr)
    raise SystemExit(1)

print(f"SD_FONT_UI_OVERFLOW_CACHE_OK capacity={capacity}")
