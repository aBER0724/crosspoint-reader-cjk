#!/usr/bin/env python3
"""Ensure paragraph layout refreshes the bounded SD advance cache."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "lib" / "GfxRenderer" / "GfxRenderer.h"
SOURCE = ROOT / "lib" / "GfxRenderer" / "GfxRenderer.cpp"
PARSED = ROOT / "lib" / "Epub" / "Epub" / "ParsedText.cpp"


def main() -> int:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    parsed = PARSED.read_text(encoding="utf-8")
    required = [
        (header, "void resetSdCardFontAdvances(int fontId) const;"),
        (source, "void GfxRenderer::resetSdCardFontAdvances(const int fontId) const {"),
        (source, "it->second->clearPersistentCache();"),
        (parsed, "renderer.resetSdCardFontAdvances(fontId);"),
        (parsed, "The bounded advance table is paragraph-local for SD fonts"),
    ]
    failures = [f"missing required snippet: {needle}" for content, needle in required if needle not in content]
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print("SD_FONT_PARAGRAPH_ADVANCE_CACHE_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
