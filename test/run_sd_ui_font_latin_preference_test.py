#!/usr/bin/env python3
"""Configured SD UI fonts must be preferred for fully covered Latin text."""
from pathlib import Path
import re
import sys

source = Path("lib/GfxRenderer/GfxRenderer.cpp").read_text()
match = re.search(r"int GfxRenderer::resolveTextFontId\([^\{]+\{(?P<body>.*?)\n\}", source, re.S)
if not match:
    print("resolveTextFontId not found", file=sys.stderr)
    raise SystemExit(1)
body = match.group("body")
if "fallbackCoversAll" not in body:
    print("configured SD UI font is not preferred for complete string coverage", file=sys.stderr)
    raise SystemExit(1)
if "return fallbackFontId" not in body:
    print("configured SD UI font can never replace the built-in font", file=sys.stderr)
    raise SystemExit(1)
if "utf8IsCjkCodepoint" not in body:
    print("legacy CJK fallback behavior was removed", file=sys.stderr)
    raise SystemExit(1)
print("SD_UI_FONT_LATIN_PREFERENCE_OK")
