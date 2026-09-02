#!/usr/bin/env python3
"""Ensure drawText reuses its resolved glyph instead of a second lookup."""
from pathlib import Path
import re
import sys

source = Path("lib/GfxRenderer/GfxRenderer.cpp").read_text()
normal = re.search(r"static void renderCharImpl\([^\{]+\{(?P<body>.*?)\n\}", source, re.S)
scaled = re.search(r"static void renderCharScaled\([^\{]+\{(?P<body>.*?)\n\}", source, re.S)
if not normal or not scaled:
    print("render helpers not found", file=sys.stderr)
    raise SystemExit(1)
for name, body in (("renderCharImpl", normal.group("body")), ("renderCharScaled", scaled.group("body"))):
    if "fontFamily.getGlyph(cp, style)" in body:
        print(f"{name} still performs a second glyph lookup", file=sys.stderr)
        raise SystemExit(1)
if "renderCharImpl<TextRotation::None>(*this, renderMode, font, glyph" not in source:
    print("drawText does not pass its resolved glyph to renderCharImpl", file=sys.stderr)
    raise SystemExit(1)
print("GFX_DRAW_TEXT_GLYPH_REUSE_OK")
