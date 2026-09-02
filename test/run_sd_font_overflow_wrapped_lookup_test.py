#!/usr/bin/env python3
"""Overflow glyph lookup must handle wrapped/evicted ring slots."""
from pathlib import Path
import sys

source = Path("lib/EpdFont/SdCardFont.cpp").read_text()
start = source.find("bool SdCardFont::isOverflowGlyph")
end = source.find("SdCardFont* SdCardFont::fromMissCtx", start)
body = source[start:end]
if body.count("i < OVERFLOW_CAPACITY") < 2:
    print("overflow glyph/bitmap lookup still assumes contiguous occupied slots", file=sys.stderr)
    raise SystemExit(1)
print("SD_FONT_OVERFLOW_WRAPPED_LOOKUP_OK")
