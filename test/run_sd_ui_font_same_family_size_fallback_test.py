#!/usr/bin/env python3
"""Failed UI size loads reuse a configured-family face instead of built-in fonts."""
from pathlib import Path
import sys

source = Path("src/SdCardFontSystem.cpp").read_text()
required = [
    "int nearestLoadedFontId = 0;",
    "renderer.setFallbackFont(ui.fontId, nearestLoadedFontId);",
    "Reusing SD UI font",
]
for token in required:
    if token not in source:
        print(f"missing same-family UI size fallback: {token}", file=sys.stderr)
        raise SystemExit(1)
print("SD_UI_FONT_SAME_FAMILY_SIZE_FALLBACK_OK")
