#!/usr/bin/env python3
"""Manifest heap guard accepts the measured successful 21,492-byte TLS arena."""
from pathlib import Path
import sys

source = Path("src/activities/settings/FontDownloadActivity.cpp").read_text()
if "MIN_NETWORK_MAX_ALLOC_BYTES = 21492" not in source:
    print("manifest guard does not use measured successful max-allocation threshold", file=sys.stderr)
    raise SystemExit(1)
if "ESP.getMaxAllocHeap() >= MIN_NETWORK_MAX_ALLOC_BYTES" not in source:
    print("manifest guard does not apply the measured threshold", file=sys.stderr)
    raise SystemExit(1)
print("FONT_MANIFEST_TLS_HEAP_THRESHOLD_OK")
