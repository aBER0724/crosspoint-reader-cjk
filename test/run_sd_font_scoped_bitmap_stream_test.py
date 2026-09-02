#!/usr/bin/env python3
"""Require drawText to scope, and never retain ownership of, an SD bitmap stream."""
from pathlib import Path
import re
import sys

renderer = Path("lib/GfxRenderer/GfxRenderer.cpp").read_text()
font_h = Path("lib/EpdFont/SdCardFont.h").read_text()
font_cpp = Path("lib/EpdFont/SdCardFont.cpp").read_text()

match = re.search(r"bool GfxRenderer::drawText\([^\{]+\{(?P<body>.*?)\n\}", renderer, re.S)
if not match:
    print("drawText not found", file=sys.stderr)
    raise SystemExit(1)
body = match.group("body")
for token in ("HalFile bitmapFile", "beginOverflowRead", "endOverflowRead"):
    if token not in body:
        print(f"drawText missing scoped bitmap stream token: {token}", file=sys.stderr)
        raise SystemExit(1)
if "HalFile overflowFile_" in font_h:
    print("SdCardFont owns a resident overflow file", file=sys.stderr)
    raise SystemExit(1)
if "HalFile* overflowFile_" not in font_h:
    print("SdCardFont does not expose a non-owning scoped overflow stream", file=sys.stderr)
    raise SystemExit(1)
miss = re.search(r"SdCardFont::onGlyphMiss\([^\{]+\{(?P<body>.*?)\n\}", font_cpp, re.S)
if not miss or "overflowFile_" not in miss.group("body"):
    print("onGlyphMiss does not reuse the scoped overflow stream", file=sys.stderr)
    raise SystemExit(1)
print("SD_FONT_SCOPED_BITMAP_STREAM_OK")
