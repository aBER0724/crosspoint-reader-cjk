#!/usr/bin/env python3
"""Guard FileBrowser path truncation against linear repeated suffix measurements."""
from pathlib import Path
import sys

source = Path("src/activities/home/FileBrowserActivity.cpp").read_text()
start = source.index("// Full path display")
end = source.index("// Help text", start)
body = source[start:end]
if "while (*p)" in body:
    print("FileBrowser path still measures every UTF-8 suffix", file=sys.stderr)
    raise SystemExit(1)
if "byteOffsets" not in body or "low" not in body or "high" not in body:
    print("FileBrowser path does not use bounded suffix search", file=sys.stderr)
    raise SystemExit(1)
print("FILE_BROWSER_PATH_BINARY_SEARCH_OK")
