#!/usr/bin/env python3
"""Guard overflow bitmap misses against rereading cached glyph metadata."""
from pathlib import Path
import sys

header = Path("lib/EpdFont/SdCardFont.h").read_text()
source = Path("lib/EpdFont/SdCardFont.cpp").read_text()
required = [
    "bool findCachedGlyphMetrics(",
    "self->findCachedGlyphMetrics(codepoint, styleIdx, &tempGlyph)",
]
for needle in required:
    if needle not in header + source:
        print(f"missing overflow metric reuse: {needle}", file=sys.stderr)
        raise SystemExit(1)
print("SD_FONT_OVERFLOW_METRIC_REUSE_OK")
