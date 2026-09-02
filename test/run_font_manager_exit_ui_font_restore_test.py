#!/usr/bin/env python3
"""Leaving font manager must restore configured SD UI fonts before settings allocations."""
from pathlib import Path
import sys

activity = Path("src/activities/settings/FontDownloadActivity.cpp").read_text()
start = activity.find("void FontDownloadActivity::onExit()")
end = activity.find("void FontDownloadActivity::onWifiSelectionComplete", start)
if start < 0 or end < 0:
    print("font manager exit lifecycle not found", file=sys.stderr)
    raise SystemExit(1)
exit_body = activity[start:end]
if "sdFontSystem.markResidentFontsDirty()" not in exit_body:
    print("font manager exit does not invalidate resident font state", file=sys.stderr)
    raise SystemExit(1)
if "sdFontSystem.ensureLoaded(renderer)" in exit_body:
    print("font manager restores fonts before parent settings allocations", file=sys.stderr)
    raise SystemExit(1)

settings = Path("src/activities/settings/SettingsActivity.cpp").read_text()
case_start = settings.find("case SettingAction::DownloadFonts:")
case_end = settings.find("case SettingAction::FontRepositories:", case_start)
if case_start < 0 or case_end < 0:
    print("manage-fonts result handler not found", file=sys.stderr)
    raise SystemExit(1)
handler = settings[case_start:case_end]
restore = handler.find("sdFontSystem.ensureLoaded(renderer)")
rebuild = handler.find("rebuildSettingsLists()")
if restore < 0 or (rebuild >= 0 and rebuild < restore):
    print("configured UI fonts are not restored before settings allocations", file=sys.stderr)
    raise SystemExit(1)

print("FONT_MANAGER_EXIT_UI_FONT_RESTORE_OK")
