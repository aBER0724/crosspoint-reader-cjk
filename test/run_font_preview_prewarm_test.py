#!/usr/bin/env python3
"""Contract checks for complete SD-font preview prewarming."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/activities/settings/FontDownloadActivity.cpp").read_text(encoding="utf-8")


def require(needle: str) -> None:
    if needle not in SOURCE:
        raise AssertionError(f"missing contract: {needle}")


require("for (const char* sample : PREVIEW_SAMPLE_LINES)")
require("\\xe6\\x98\\x8e\\xe6\\x9c\\x9d\\xe9\\xbb\\x91\\xe4\\xbd\\x93\\xe6\\xa5\\xb7\\xe4\\xbd\\x93")
require("\\xe9\\x96\\xb1\\xe8\\xae\\x80\\xe9\\xa0\\x90\\xe8\\xa6\\xbd\\xe5\\xad\\x97\\xe9\\xab\\x94")
require("\\xe3\\x81\\x84\\xe3\\x81\\x86\\xe3\\x81\\x88\\xe3\\x81\\x8a")
require("\\xe3\\x82\\xa4\\xe3\\x82\\xa6\\xe3\\x82\\xa8\\xe3\\x82\\xaa")
require('cache->resetStats();')
require('cache->logStats("font-preview");')
require('cache->prewarmCache(previewFontId_, sample, 0x01)')

SD_FONT_SOURCE = (ROOT / "lib/EpdFont/SdCardFont.cpp").read_text(encoding="utf-8")
FONT_CACHE_SOURCE = (ROOT / "lib/GfxRenderer/FontCacheManager.cpp").read_text(encoding="utf-8")
GFX_SOURCE = (ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text(encoding="utf-8")
if "textCodepointCount + 1 + possibleLigatureOutputs" not in SD_FONT_SOURCE:
    raise AssertionError("preview prewarm must size its codepoint buffer to the input")
if "if (missed < 0) return false;" not in FONT_CACHE_SOURCE:
    raise AssertionError("SD font prewarm allocation failures must propagate")
if "if (styleResult < 0) return finish(PREWARM_FAILED);" not in SD_FONT_SOURCE:
    raise AssertionError("per-style prewarm failures must propagate")
registered_font_guard = "if (sdCardFonts_.count(fontId) != 0) {\n    return fontId;\n  }"
external_font_guard = "if (fontId < 0 && isReaderFont(fontId))"
if registered_font_guard not in GFX_SOURCE:
    raise AssertionError("registered negative SD font IDs must remain authoritative")
if GFX_SOURCE.index(registered_font_guard) > GFX_SOURCE.index(external_font_guard):
    raise AssertionError("registered SD font IDs must be checked before synthetic external IDs")
reader_guard = "if (sdCardFonts_.count(fontId) != 0) {\n    return false;\n  }"
if reader_guard not in GFX_SOURCE:
    raise AssertionError("registered SD fonts must not enter external reader rendering")

print("FONT_PREVIEW_PREWARM_OK")
