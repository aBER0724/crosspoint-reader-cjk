#!/usr/bin/env python3
"""Contract checks for metric-only SD-font UI measurement."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "lib/EpdFont/SdCardFont.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "lib/EpdFont/SdCardFont.cpp").read_text(encoding="utf-8")
RENDERER = (ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text(encoding="utf-8")

required = [
    (HEADER, "bool getGlyphMetrics(uint32_t codepoint, uint8_t style, EpdGlyph* outGlyph, HalFile* metricsFile = nullptr);"),
    (HEADER, "METRICS_CACHE_CAPACITY"),
    (SOURCE, "bool SdCardFont::getGlyphMetrics("),
    (SOURCE, "file->read(reinterpret_cast<uint8_t*>(outGlyph), sizeof(EpdGlyph))"),
    (RENDERER, "sdIt->second->getGlyphMetrics(cp, styleIdx, &metricGlyph, &metricsFile)"),
    (RENDERER, "if (sdCardFonts_.count(resolvedFontId) != 0)"),
]

for content, needle in required:
    if needle not in content:
        print(f"missing metric-only measurement contract: {needle}", file=sys.stderr)
        raise SystemExit(1)

width_start = RENDERER.index("int GfxRenderer::getTextWidth(")
width_end = RENDERER.index("\n}", width_start)
width_body = RENDERER[width_start:width_end]
metric_branch = width_body.index("if (sdCardFonts_.count(resolvedFontId) != 0)")
glyph_preflight = width_body.index("bool needsFallback = false;")
if metric_branch > glyph_preflight:
    print("SD metric-only branch must run before bitmap-loading glyph preflight", file=sys.stderr)
    raise SystemExit(1)

print("SD_FONT_UI_METRICS_ONLY_OK")
