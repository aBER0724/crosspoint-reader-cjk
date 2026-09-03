#!/usr/bin/env python3
"""Contract checks for the low-memory BMP coverage index."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "lib/EpdFont/SdCardFont.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "lib/EpdFont/SdCardFont.cpp").read_text(encoding="utf-8")


def require(text: str, needle: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing contract: {needle}")


require(HEADER, "BMP_PAGE_MAP_WORDS = BMP_PAGE_COUNT / 32")
require(HEADER, "uint32_t bmpCoveredPages[BMP_PAGE_MAP_WORDS]")
require(HEADER, "uint32_t* bmpCoverageData")
require(HEADER, "uint16_t bmpMiniBitmapCapacity")
require(HEADER, "bool miniBitmapUsesCoverageArena")
require(SOURCE, "compactIntervalBytes > BMP_INTERVAL_TABLE_MAX_BYTES")
require(SOURCE, "static constexpr uint32_t MAX_INTERVALS = MAX_GLYPHS")
require(SOURCE, "s.header.intervalCount > s.header.glyphCount")
require(SOURCE, "const size_t offsetWordCount = (s.bmpCoveragePageCount + 1) / 2")
require(SOURCE, "s.bmpMiniBitmapCapacity = 0")
require(SOURCE, "uint32_t[indexWordCount]()")
require(SOURCE, "glyphOffset != s.header.glyphCount")
require(SOURCE, "if (s.intervalsAreBmpCoverage)")
require(SOURCE, "totalBitmapSize <= s.bmpMiniBitmapCapacity")
require(SOURCE, "s.miniBitmapUsesCoverageArena = true")
require(SOURCE, "if (s.intervalsAreBmpCoverage && totalBitmapSize <= s.bmpMiniBitmapCapacity)")
require(SOURCE, "ensureArrayCapacity(s.miniBitmap, s.miniBitmapCapacity, totalBitmapSize)")
require(SOURCE, "const uint32_t precedingMask = targetMask - 1u")
require(SOURCE, "delete[] s.bmpCoverageData")

print("SD_FONT_COVERAGE_INDEX_OK")
