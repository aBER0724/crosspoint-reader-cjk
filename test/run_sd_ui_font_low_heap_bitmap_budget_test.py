#!/usr/bin/env python3
"""SD UI bitmap cache preserves a low-memory Wi-Fi/network reserve."""
from pathlib import Path
import sys

header = Path("lib/EpdFont/SdCardFont.h").read_text()
source = Path("lib/EpdFont/SdCardFont.cpp").read_text()
required = [
    "OVERFLOW_LOW_HEAP_RESERVE_BYTES",
    "overflowBitmapBudget()",
    "heap_caps_get_free_size(MALLOC_CAP_8BIT)",
]
for token in required:
    if token not in header + source:
        print(f"missing adaptive overflow budget: {token}", file=sys.stderr)
        raise SystemExit(1)
if "self->overflowBitmapBudget()" not in source:
    print("glyph eviction does not use adaptive budget", file=sys.stderr)
    raise SystemExit(1)
print("SD_UI_FONT_LOW_HEAP_BITMAP_BUDGET_OK")
