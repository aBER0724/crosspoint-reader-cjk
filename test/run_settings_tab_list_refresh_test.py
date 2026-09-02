#!/usr/bin/env python3
"""Settings tab/list transitions must refresh both changed regions."""
from pathlib import Path
import sys

source = Path("src/activities/settings/SettingsActivity.cpp").read_text()
required = [
    "const int listSelection = selectedSettingIndex == 0 ? lastRenderedSettingIndex : selectedSettingIndex;",
    "const int visibleRow = std::max(0, listSelection - 1) % pageItems;",
    "listTop + (visibleRow + 1) * rowStep - tabTop",
]
for token in required:
    if token not in source:
        print(f"missing settings transition refresh logic: {token}", file=sys.stderr)
        raise SystemExit(1)
print("SETTINGS_TAB_LIST_REFRESH_OK")
