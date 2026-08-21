#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOME_ACTIVITY_CPP = ROOT / "src" / "activities" / "home" / "HomeActivity.cpp"
HOME_ACTIVITY_H = ROOT / "src" / "activities" / "home" / "HomeActivity.h"
PLATFORMIO_INI = ROOT / "platformio.ini"


def main() -> int:
    source = HOME_ACTIVITY_CPP.read_text(encoding="utf-8")
    header = HOME_ACTIVITY_H.read_text(encoding="utf-8")
    platformio = PLATFORMIO_INI.read_text(encoding="utf-8")

    required = [
        ("HomeActivity.cpp", "canUseMenuOnlyPartialUpdate("),
        ("HomeActivity.cpp", "setPartialUpdateRect("),
        ("HomeActivity.cpp", "menuOnlyPartialUpdate"),
        ("HomeActivity.cpp", "isCoverSelectionIndex("),
        ("HomeActivity.cpp", "GUI.getHomeMenuDirtyRect("),
        ("HomeActivity.cpp", "} else if (!firstRenderDone) {"),
        ("HomeActivity.cpp", "renderer.waitRefreshComplete();"),
        ("HomeActivity.cpp", "const int renderedSelectorIndex = selectorIndex;"),
        ("HomeActivity.h", "fullRedrawRequired"),
        ("HomeActivity.h", "lastRenderedSelectorIndex"),
        ("HomeActivity.h", "canUseMenuOnlyPartialUpdate"),
    ]

    failures = []
    for filename, needle in required:
        text = source if filename.endswith(".cpp") else header
        if needle not in text:
            failures.append(f"{filename} is missing {needle!r}")

    forbidden = [
        ("HomeActivity.cpp", "setMenuPartialUpdateIfSafe("),
        ("HomeActivity.h", "setMenuPartialUpdateIfSafe"),
    ]
    for filename, needle in forbidden:
        text = source if filename.endswith(".cpp") else header
        if needle in text:
            failures.append(f"{filename} still contains {needle!r}")

    if "-DFREEINK_X4_FAST_DU_SHORTCUT=1" not in platformio:
        failures.append("platformio.ini must enable FREEINK_X4_FAST_DU_SHORTCUT for X4 builds")

    if failures:
        print("HomeActivity must keep the safe menu-only partial refresh path.")
        for failure in failures:
            print(f"- {failure}")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
