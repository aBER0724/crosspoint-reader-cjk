#!/usr/bin/env python3
"""Text settings locale routing must compile with filtered release language enums."""
from pathlib import Path
import sys

source = Path("src/activities/settings/TextSettingsActivity.cpp").read_text()

for forbidden in (
    "Language::CHINESE_TRADITIONAL",
    "Language::JAPANESE",
    "Language::CHINESE_SIMPLIFIED",
):
    if forbidden in source:
        print(f"filtered language enum referenced directly: {forbidden}", file=sys.stderr)
        raise SystemExit(1)

required = (
    "LANGUAGE_CODES[languageIndex]",
    'std::strcmp(languageCode, "CHINESE_TRADITIONAL")',
    'std::strcmp(languageCode, "JAPANESE")',
    'return "NotoSansTC";',
    'return "NotoSansJP";',
    'return "NotoSansSC";',
)
for token in required:
    if token not in source:
        print(f"missing filtered-language-safe Noto Sans routing: {token}", file=sys.stderr)
        raise SystemExit(1)

print("TEXT_SETTINGS_FILTERED_LANGUAGE_BUILD_OK")
