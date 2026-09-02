#!/usr/bin/env python3
"""Guard truncation against quadratic repeated whole-prefix measurements."""
from pathlib import Path
import re
import sys

source = Path("lib/GfxRenderer/GfxRenderer.cpp").read_text()
match = re.search(r"std::string GfxRenderer::truncatedText\([^\{]+\{(?P<body>.*?)\n\}", source, re.S)
if not match:
    print("truncatedText not found", file=sys.stderr)
    raise SystemExit(1)
body = match.group("body")
if "while (!item.empty()" in body or "utf8RemoveLastChar(item)" in body:
    print("truncatedText still removes and remeasures one codepoint at a time", file=sys.stderr)
    raise SystemExit(1)
if "byteOffsets" not in body or "low" not in body or "high" not in body:
    print("truncatedText does not use bounded UTF-8 prefix search", file=sys.stderr)
    raise SystemExit(1)
print("GFX_TRUNCATED_TEXT_BINARY_SEARCH_OK")
