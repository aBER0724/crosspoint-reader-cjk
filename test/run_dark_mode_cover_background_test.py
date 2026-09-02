#!/usr/bin/env python3
from pathlib import Path
import sys

source = Path("lib/GfxRenderer/GfxRenderer.cpp").read_text()

expected = "const bool fillBlack = (C == Color::Black) != (darkMode && !skipDarkModeForImages);"
if expected not in source:
    print("solid fill path does not honor image dark-mode bypass", file=sys.stderr)
    sys.exit(1)

if "fillRect(x, y, scaledWidth, scaledHeight, false);" not in source:
    print("general bitmap path does not pre-fill its white background", file=sys.stderr)
    sys.exit(1)

if "fillRect(x, y, displayWidth, displayHeight, false);" not in source:
    print("1-bit bitmap path does not pre-fill its white background", file=sys.stderr)
    sys.exit(1)

if source.count("skipDarkModeForImages = true;") < 2:
    print("bitmap paths do not enable the image dark-mode bypass", file=sys.stderr)
    sys.exit(1)

print("DARK_MODE_COVER_BACKGROUND_OK")
