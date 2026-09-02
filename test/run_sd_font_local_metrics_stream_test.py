#!/usr/bin/env python3
"""Guard metric stream reuse without a resident HalFile per SD font."""
from pathlib import Path
import sys

header = Path("lib/EpdFont/SdCardFont.h").read_text()
renderer = Path("lib/GfxRenderer/GfxRenderer.cpp").read_text()
if "HalFile metricsFile_;" in header:
    print("SD font still owns a resident metrics HalFile", file=sys.stderr)
    raise SystemExit(1)
required = [
    "HalFile* metricsFile = nullptr",
    "HalFile metricsFile;",
    "&metricGlyph, &metricsFile",
]
for needle in required:
    if needle not in header + renderer:
        print(f"missing local metric stream reuse: {needle}", file=sys.stderr)
        raise SystemExit(1)
print("SD_FONT_LOCAL_METRICS_STREAM_OK")
