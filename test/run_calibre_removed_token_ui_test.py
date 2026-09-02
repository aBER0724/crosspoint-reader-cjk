#!/usr/bin/env python3
"""Calibre connection screen must not reference the removed web admin token API."""
from pathlib import Path
import sys

source = Path("src/activities/network/CalibreConnectActivity.cpp").read_text()
for obsolete in ("getAdminToken", "appendAdminToken", "STR_TOKEN_PREFIX"):
    if obsolete in source:
        print(f"obsolete web admin token reference remains: {obsolete}", file=sys.stderr)
        raise SystemExit(1)
if 'const std::string serverUrl = "http://" + connectedIP + "/";' not in source:
    print("plain Calibre server URL is missing", file=sys.stderr)
    raise SystemExit(1)
print("CALIBRE_REMOVED_TOKEN_UI_OK")
