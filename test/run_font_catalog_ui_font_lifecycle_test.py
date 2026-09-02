#!/usr/bin/env python3
"""Font catalog must restore configured UI fonts after network cleanup."""
from pathlib import Path
import sys

source = Path("src/activities/settings/FontDownloadActivity.cpp").read_text()
cleanup = source.find("Heap after WiFi/cache cleanup before family list")
family_state = source.find("state_ = FAMILY_LIST", cleanup)
restore = source.find("sdFontSystem.ensureLoaded(renderer)", cleanup, family_state)
if cleanup < 0 or family_state < 0:
    print("font catalog post-network transition not found", file=sys.stderr)
    raise SystemExit(1)
if restore < 0:
    print("configured UI fonts are not restored before rendering the family list", file=sys.stderr)
    raise SystemExit(1)

begin = source.find("void FontDownloadActivity::beginNetworkTransfer")
end = source.find("bool FontDownloadActivity::endNetworkTransfer", begin)
release = source.find("sdFontSystem.releaseResidentFonts(renderer)", begin, end)
if release < 0:
    print("configured UI fonts are not released again before font-file TLS", file=sys.stderr)
    raise SystemExit(1)

next_section = source.find("void FontDownloadActivity::updateDownloadProgress", end)
transfer_restore = source.find("sdFontSystem.ensureLoaded(renderer)", end, next_section)
if transfer_restore < 0:
    print("configured UI fonts are not restored after a font-file transfer", file=sys.stderr)
    raise SystemExit(1)

print("FONT_CATALOG_UI_FONT_LIFECYCLE_OK")
