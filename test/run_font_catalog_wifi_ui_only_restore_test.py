#!/usr/bin/env python3
"""Wi-Fi selector restores only UI fonts so reader allocations cannot starve 12 pt."""
from pathlib import Path
import sys

header = Path("src/SdCardFontSystem.h").read_text()
source = Path("src/SdCardFontSystem.cpp").read_text()
activity = Path("src/activities/settings/FontDownloadActivity.cpp").read_text()

if "void ensureUiLoaded(GfxRenderer& renderer);" not in header:
    print("UI-only resident font restore API missing", file=sys.stderr)
    raise SystemExit(1)
if "void SdCardFontSystem::ensureUiLoaded(GfxRenderer& renderer)" not in source:
    print("UI-only resident font restore implementation missing", file=sys.stderr)
    raise SystemExit(1)

start = activity.find("void FontDownloadActivity::onEnter()")
end = activity.find("void FontDownloadActivity::onExit()", start)
body = activity[start:end]
if "sdFontSystem.ensureUiLoaded(renderer);" not in body:
    print("font catalog Wi-Fi selector does not use UI-only restore", file=sys.stderr)
    raise SystemExit(1)
if "sdFontSystem.ensureLoaded(renderer);" in body:
    print("font catalog Wi-Fi selector still restores reader fonts", file=sys.stderr)
    raise SystemExit(1)

print("FONT_CATALOG_WIFI_UI_ONLY_RESTORE_OK")
