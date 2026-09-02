#!/usr/bin/env python3
"""Blocking reader index work must wait until the indexing popup reaches the panel."""
from pathlib import Path
import sys

files = {
    "epub": Path("src/activities/reader/EpubReaderActivity.cpp").read_text(),
    "reader": Path("src/activities/reader/ReaderActivity.cpp").read_text(),
    "txt": Path("src/activities/reader/TxtReaderActivity.cpp").read_text(),
}
if files["epub"].count("renderer.waitRefreshComplete();") < 3:
    print("EPUB indexing paths do not consistently wait for popup refresh", file=sys.stderr)
    raise SystemExit(1)
for name in ("reader", "txt"):
    if "renderer.waitRefreshComplete();" not in files[name]:
        print(f"{name} indexing path does not wait for popup refresh", file=sys.stderr)
        raise SystemExit(1)
print("READER_INDEXING_POPUP_WAIT_OK")
