#!/usr/bin/env python3
"""Regression checks for the file-page initialization sequence."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGE = (ROOT / "src/network/html/FilesPage.html").read_text(encoding="utf-8")

assert "const DEFAULT_ENABLE_AUTO_CROP = false;" in PAGE
assert "suppressUploadSettingsSave = true;\n      try {\n        updateQualitySettings();" not in PAGE
assert "runs before the later upload-settings declarations are initialized" in PAGE
assert PAGE.index("const DEFAULT_ENABLE_AUTO_CROP") < PAGE.index("DEFAULT_ENABLE_AUTO_CROP,")

print("Files page initialization regression checks passed")
