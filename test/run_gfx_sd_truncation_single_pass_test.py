#!/usr/bin/env python3
"""Require SD-backed truncation to measure prefixes in one scoped metrics pass."""
from pathlib import Path
import re
import sys

source = Path("lib/GfxRenderer/GfxRenderer.cpp").read_text()
match = re.search(r"std::string GfxRenderer::truncatedText\([^\{]+\{(?P<body>.*?)\n\}", source, re.S)
if not match:
    print("truncatedText not found", file=sys.stderr)
    raise SystemExit(1)
body = match.group("body")
if "measureSdTextPrefix" not in body:
    print("truncatedText does not use the scoped SD prefix measurement path", file=sys.stderr)
    raise SystemExit(1)
if "HalFile metricsFile" not in body:
    print("truncatedText does not scope one reusable metrics file", file=sys.stderr)
    raise SystemExit(1)

helper = re.search(r"GfxRenderer::measureSdTextPrefix\([^\{]+\{(?P<body>.*?)\n\}", source, re.S)
if not helper:
    print("measureSdTextPrefix helper not found", file=sys.stderr)
    raise SystemExit(1)
helper_body = helper.group("body")
if "getGlyphMetrics" not in helper_body or "prefixWidths" not in helper_body:
    print("SD prefix helper does not accumulate metrics-only prefix widths", file=sys.stderr)
    raise SystemExit(1)
if "getTextWidth(" in helper_body:
    print("SD prefix helper remeasures whole candidate strings", file=sys.stderr)
    raise SystemExit(1)
print("GFX_SD_TRUNCATION_SINGLE_PASS_OK")
