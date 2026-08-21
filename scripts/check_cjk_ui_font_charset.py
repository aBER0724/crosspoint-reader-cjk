#!/usr/bin/env python3
"""Verify the built-in CJK UI font covers every shipping CJK translation."""

import re
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from generate_cjk_ui_font import build_ui_chars, collect_translation_chars, get_unique_chars  # noqa: E402


TRANSLATIONS_DIR = ROOT / "lib" / "I18n" / "translations"
FONT_HEADER = ROOT / "lib" / "GfxRenderer" / "cjk_ui_font_20.h"
SHIPPING_CJK_LANGUAGES = ["CHINESE_SIMPLIFIED", "CHINESE_TRADITIONAL", "JAPANESE"]


def read_generated_codepoints(path: Path) -> set[int]:
    text = path.read_text(encoding="utf-8")
    match = re.search(r"CJK_UI_CODEPOINTS\[\].*?=\s*\{(.*?)\};", text, re.DOTALL)
    if not match:
        raise ValueError(f"Could not find CJK_UI_CODEPOINTS in {path}")
    return {int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)", match.group(1))}


def verify_shipping_translations() -> bool:
    generated = read_generated_codepoints(FONT_HEADER)
    failed = False

    for language in SHIPPING_CJK_LANGUAGES:
        required = {
            ord(char)
            for char in collect_translation_chars(TRANSLATIONS_DIR, [language])
            if ord(char) >= 0x80
        }
        missing = sorted(required - generated)
        if not missing:
            continue

        failed = True
        print(f"{language}: built-in CJK UI font is missing {len(missing)} translated characters:")
        # Keep diagnostics printable on Windows consoles that still use a
        # non-UTF-8 code page. The codepoint is sufficient to locate the glyph.
        print("  " + " ".join(f"U+{codepoint:04X}" for codepoint in missing))

    if not failed:
        print(f"Built-in CJK UI font covers all shipping translations ({len(generated)} glyphs).")
    return not failed


def write_translation(path: Path, language_name: str, language_code: str, value: str) -> None:
    path.write_text(
        "\n".join(
            [
                f'_language_name: "{language_name}"',
                f'_language_code: "{language_code}"',
                'STR_CROSSPOINT: "CrossPoint"',
                f'STR_TEST_ONLY: "{value}"',
                "",
            ]
        ),
        encoding="utf-8",
    )


def main() -> int:
    if not verify_shipping_translations():
        return 1

    # Keep a focused contract test for language filtering. It catches a generator
    # regression even when the checked-in header still happens to cover all text.
    with tempfile.TemporaryDirectory() as tmpdir:
        translations_dir = Path(tmpdir)
        write_translation(translations_dir / "english.yaml", "English", "EN", "English")
        write_translation(translations_dir / "chinese_simplified.yaml", "简体中文", "CHINESE_SIMPLIFIED", "鱻龘")
        write_translation(translations_dir / "japanese.yaml", "日本語", "JAPANESE", "麒麟")

        chars = set(get_unique_chars(build_ui_chars(translations_dir, ["CHINESE_SIMPLIFIED"])))
        expected = {"鱻", "龘"}
        unexpected = {"麒", "麟"}

        if not expected.issubset(chars):
            print("Generated UI charset missed selected translation characters.")
            print(f"Missing: {sorted(expected - chars)}")
            return 1

        if chars.intersection(unexpected):
            print("Generated UI charset included filtered-out language characters.")
            print(f"Unexpected: {sorted(chars.intersection(unexpected))}")
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
