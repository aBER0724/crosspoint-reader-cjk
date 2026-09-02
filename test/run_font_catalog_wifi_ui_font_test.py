#!/usr/bin/env python3
"""Font catalog Wi-Fi selector must use the configured SD UI font."""
from pathlib import Path
import sys

source = Path("src/activities/settings/FontDownloadActivity.cpp").read_text()
start = source.find("void FontDownloadActivity::onEnter()")
end = source.find("void FontDownloadActivity::onExit()", start)
if start < 0 or end < 0:
    print("font catalog entry lifecycle not found", file=sys.stderr)
    raise SystemExit(1)
body = source[start:end]
release = body.find("sdFontSystem.releaseResidentFonts(renderer)")
wifi = body.find("if (!WiFi.mode(WIFI_STA))")
restore = body.find("sdFontSystem.ensureUiLoaded(renderer)", wifi)
selector = body.find("std::make_unique<WifiSelectionActivity>", wifi)
if min(release, wifi, restore, selector) < 0 or not (release < wifi < restore < selector):
    print("configured UI font is not restored for the Wi-Fi selector", file=sys.stderr)
    raise SystemExit(1)

transfer_start = source.find("void FontDownloadActivity::beginNetworkTransfer")
transfer_end = source.find("bool FontDownloadActivity::endNetworkTransfer", transfer_start)
if source.find("sdFontSystem.releaseResidentFonts(renderer)", transfer_start, transfer_end) < 0:
    print("configured UI font is not released again before TLS", file=sys.stderr)
    raise SystemExit(1)

print("FONT_CATALOG_WIFI_UI_FONT_OK")
