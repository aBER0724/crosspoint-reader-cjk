#!/usr/bin/env python3
"""Guard drawList against measuring the same row value twice."""
from pathlib import Path
import re
import sys

source = Path("src/components/themes/BaseTheme.cpp").read_text()
start = source.index("void BaseTheme::drawList(")
end = source.index("\n}\n\nvoid BaseTheme::drawHeader", start)
body = source[start:end]
if body.count("getTextWidth(UI_10_FONT_ID, valueText.c_str())") != 1:
    print("drawList does not reuse the row value width", file=sys.stderr)
    raise SystemExit(1)
if "int valueTextWidth = 0;" not in body:
    print("drawList has no retained row value width", file=sys.stderr)
    raise SystemExit(1)
print("DRAW_LIST_VALUE_WIDTH_REUSE_OK")
