#!/usr/bin/env python3
"""Regression checks for font-catalog network memory preparation."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ACTIVITY = (ROOT / "src/activities/settings/FontDownloadActivity.cpp").read_text()
SYSTEM_HEADER = (ROOT / "src/SdCardFontSystem.h").read_text()
SYSTEM_SOURCE = (ROOT / "src/SdCardFontSystem.cpp").read_text()

release_call = "sdFontSystem.releaseResidentFonts(renderer);"
wifi_start = "if (!WiFi.mode(WIFI_STA)) {"
assert release_call in ACTIVITY, "font catalog must release resident SD fonts before starting Wi-Fi"
assert ACTIVITY.index(release_call) < ACTIVITY.index(wifi_start), "resident SD fonts must be released before Wi-Fi starts"

assert "void releaseResidentFonts(GfxRenderer& renderer);" in SYSTEM_HEADER
assert "void SdCardFontSystem::releaseResidentFonts(GfxRenderer& renderer)" in SYSTEM_SOURCE
assert "manager_.unloadAll(renderer);" in SYSTEM_SOURCE
assert "residentFontsDirty_.store(true" in SYSTEM_SOURCE

on_exit_start = ACTIVITY.index("void FontDownloadActivity::onExit()")
on_exit_end = ACTIVITY.index("void FontDownloadActivity::onWifiSelectionComplete", on_exit_start)
on_exit = ACTIVITY[on_exit_start:on_exit_end]
wifi_off = "WiFi.mode(WIFI_OFF);"
dirty_call = "sdFontSystem.markResidentFontsDirty();"
assert dirty_call in on_exit, "leaving the catalog must invalidate resident SD font state"
assert on_exit.index(wifi_off) < on_exit.index(dirty_call), "Wi-Fi must be shut down before font restoration is scheduled"

settings_source = (ROOT / "src/activities/settings/SettingsActivity.cpp").read_text()
case_start = settings_source.index("case SettingAction::DownloadFonts:")
case_end = settings_source.index("case SettingAction::FontRepositories:", case_start)
handler = settings_source[case_start:case_end]
restore_call = "sdFontSystem.ensureLoaded(renderer);"
assert restore_call in handler, "parent settings must restore configured SD fonts after catalog exit"
rebuild_pos = handler.find("rebuildSettingsLists();")
assert rebuild_pos < 0 or handler.index(restore_call) < rebuild_pos, (
    "resident fonts must be restored before optional settings-list allocations fragment the heap"
)
print("font network memory regression checks passed")
