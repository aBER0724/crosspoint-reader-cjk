#!/usr/bin/env python3
"""Regression checks for consistent Web navigation across all main pages."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGES = {
    "HomePage.html": "/",
    "FilesPage.html": "/files",
    "SettingsPage.html": "/settings",
    "FontsPage.html": "/fonts",
}
NAV_ITEMS = {
    "/": "Home",
    "/files": "File Manager",
    "/settings": "Settings",
    "/fonts": "Fonts",
}

for filename, active_path in PAGES.items():
    page = (ROOT / "src/network/html" / filename).read_text(encoding="utf-8")
    nav_start = page.index('<div class="nav-links">')
    nav_end = page.index("</div>", nav_start)
    nav = page[nav_start:nav_end]

    for path, label in NAV_ITEMS.items():
        link = f'<a href="{path}"'
        assert nav.count(link) == 1, f"{filename}: missing or duplicate {label} navigation link"

    active_link = f'<a href="{active_path}" class="active">'
    assert active_link in nav, f"{filename}: active navigation item does not match {active_path}"

print("Web navigation regression checks passed")
