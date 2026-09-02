#!/usr/bin/env python3
"""Home reuses cached covers and regenerates only missing thumbnails after idle."""
from pathlib import Path
import sys

home = Path("src/activities/home/HomeActivity.cpp").read_text()
theme = Path("src/components/UITheme.cpp").read_text()
checks = {
    "Home schedules deferred cover work": "nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_IDLE_MS;" in home,
    "Home processes at most one missing cover": "requestUpdate();\n    return;" in home,
    "Home loads EPUB metadata before generation": "if (epub.load(false, true))" in home,
    "Home regenerates EPUB thumbnails": "epub.generateThumbBmp(coverHeight)" in home,
    "Home loads XTC before generation": "if (xtc.load())" in home,
    "Home regenerates XTC thumbnails": "xtc.generateThumbBmp(coverHeight)" in home,
    "Home repairs empty recent paths": "book.coverBmpPath = epub.getThumbBmpPath();" in home,
    "Home preserves cover path on generation failure":
        "RECENT_BOOKS.updateBook(book.path, book.title, book.author, \"\")" not in home,
    "Theme checks exact thumbnail": "Storage.exists(exactPath.c_str())" in theme,
    "Theme searches alternate heights": "directory.openNextFile()" in theme and "numericHeight" in theme,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    print("; ".join(failed), file=sys.stderr)
    raise SystemExit(1)
print("HOME_COVER_REGENERATION_OK")
