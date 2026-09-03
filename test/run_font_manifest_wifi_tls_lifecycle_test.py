#!/usr/bin/env python3
"""Manifest TLS keeps SD UI fonts unloaded from STA startup through fetch."""
from pathlib import Path
import sys

source = Path("src/activities/settings/FontDownloadActivity.cpp").read_text()
entry_start = source.find("void FontDownloadActivity::onEnter()")
entry_end = source.find("void FontDownloadActivity::onExit()", entry_start)
complete_start = source.find("void FontDownloadActivity::onWifiSelectionComplete")
complete_end = source.find("bool FontDownloadActivity::fetchAndParseManifests", complete_start)
if min(entry_start, entry_end, complete_start, complete_end) < 0:
    print("font catalog Wi-Fi lifecycle not found", file=sys.stderr)
    raise SystemExit(1)

entry = source[entry_start:entry_end]
complete = source[complete_start:complete_end]
release = entry.find("sdFontSystem.releaseResidentFonts(renderer)")
wifi = entry.find("if (!WiFi.mode(WIFI_STA))")
selector = entry.find("std::make_unique<WifiSelectionActivity>", wifi)
if min(release, wifi, selector) < 0 or not (release < wifi < selector):
    print("resident fonts are not released before STA and selector startup", file=sys.stderr)
    raise SystemExit(1)
if entry.find("sdFontSystem.ensureUiLoaded(renderer)", wifi, selector) >= 0:
    print("SD UI fonts are reloaded between STA startup and selector", file=sys.stderr)
    raise SystemExit(1)

transfer = complete.find("beginNetworkTransfer()")
heap_guard = complete.find("heapSufficientForNetworkTransfer()", transfer)
fetch = complete.find("fetchAndParseManifests()", transfer)
if min(transfer, heap_guard, fetch) < 0 or not (transfer < heap_guard < fetch):
    print("manifest resource release/heap guard/fetch order is invalid", file=sys.stderr)
    raise SystemExit(1)
if "WiFi.mode(WIFI_OFF)" in complete[transfer:heap_guard]:
    print("STA is unnecessarily recycled before the manifest heap guard", file=sys.stderr)
    raise SystemExit(1)

print("FONT_MANIFEST_WIFI_TLS_LIFECYCLE_OK")
