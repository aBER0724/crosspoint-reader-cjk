#!/usr/bin/env python3
"""Home must reuse existing cover thumbnails without generating in its input loop."""
from pathlib import Path
import sys

home = Path("src/activities/home/HomeActivity.cpp").read_text()
theme = Path("src/components/UITheme.cpp").read_text()
checks = {
    "Home normalizes existing thumbnail paths": "loadNextRecentCover(metrics.homeCoverHeight);" in home,
    "Home does not generate EPUB thumbnails": "generateThumbBmp" not in home,
    "Theme checks exact thumbnail": "Storage.exists(exactPath.c_str())" in theme,
    "Theme searches alternate heights": "directory.openNextFile()" in theme and "numericHeight" in theme,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    print("; ".join(failed), file=sys.stderr)
    raise SystemExit(1)
print("HOME_EXISTING_COVER_REUSE_OK")
