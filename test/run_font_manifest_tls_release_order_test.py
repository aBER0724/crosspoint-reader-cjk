#!/usr/bin/env python3
"""Wi-Fi selector fonts must be released before the manifest TLS heap guard."""
from pathlib import Path
import sys

source = Path("src/activities/settings/FontDownloadActivity.cpp").read_text()
start = source.find("void FontDownloadActivity::onWifiSelectionComplete")
end = source.find("// --- Manifest fetching ---", start)
if start < 0 or end < 0:
    print("Wi-Fi completion handler missing", file=sys.stderr)
    raise SystemExit(1)
body = source[start:end]
release = body.find("beginNetworkTransfer();")
guard = body.find("heapSufficientForNetworkTransfer()")
fetch = body.find("fetchAndParseManifests()")
if min(release, guard, fetch) < 0 or not (release < guard < fetch):
    print("manifest heap guard runs before Wi-Fi UI fonts are released", file=sys.stderr)
    raise SystemExit(1)
print("FONT_MANIFEST_TLS_RELEASE_ORDER_OK")
