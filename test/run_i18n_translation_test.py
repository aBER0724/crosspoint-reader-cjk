#!/usr/bin/env python3

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
GENERATOR = REPO_ROOT / "scripts" / "gen_i18n.py"
TRANSLATIONS = REPO_ROOT / "lib" / "I18n" / "translations"
GENERATED_FILES = (
    REPO_ROOT / "lib" / "I18n" / "I18nKeys.h",
    REPO_ROOT / "lib" / "I18n" / "I18nStrings.h",
    REPO_ROOT / "lib" / "I18n" / "I18nStrings.cpp",
)
SHIPPING_LANGUAGES = (
    "chinese_simplified.yaml",
    "chinese_traditional.yaml",
    "japanese.yaml",
)
ALLOWED_ENGLISH_VALUES = {
    "CrossPoint",
    "KOSync",
    "Lyra",
    "Noto Sans",
    "Noto Serif",
    "Open Dyslexic",
    "RoundedRaff",
    "Shift",
    "?123",
    "abc",
    "OPDS URL",
    "URL: ",
}
MOJIBAKE_MARKERS = ("\ufffd", "\u951b", "\u9225", "\u935b", "\u951f")


def load_generator_module():
    spec = importlib.util.spec_from_file_location("gen_i18n", GENERATOR)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load {GENERATOR}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ShippingTranslationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.generator = load_generator_module()
        cls.english = cls.generator.parse_yaml_file(str(TRANSLATIONS / "english.yaml"))

    def test_shipping_languages_are_complete_and_format_compatible(self):
        english_strings = {key: value for key, value in self.english.items() if key.startswith("STR_")}

        for filename in SHIPPING_LANGUAGES:
            with self.subTest(language=filename):
                translated = self.generator.parse_yaml_file(str(TRANSLATIONS / filename))
                translated_strings = {key: value for key, value in translated.items() if key.startswith("STR_")}
                self.assertEqual(set(translated_strings), set(english_strings))
                self.assertFalse([key for key, value in translated_strings.items() if not value.strip()])

                for key, english_value in english_strings.items():
                    self.assertEqual(
                        self.generator.extract_printf_placeholders(translated_strings[key]),
                        self.generator.extract_printf_placeholders(english_value),
                        f"placeholder mismatch in {filename}:{key}",
                    )

    def test_shipping_languages_have_no_mojibake_or_unreviewed_english_fallbacks(self):
        for filename in SHIPPING_LANGUAGES:
            translated = self.generator.parse_yaml_file(str(TRANSLATIONS / filename))
            with self.subTest(language=filename):
                for key, value in translated.items():
                    if not key.startswith("STR_"):
                        continue
                    self.assertFalse(any(marker in value for marker in MOJIBAKE_MARKERS), f"mojibake in {filename}:{key}")
                    if value == self.english[key]:
                        self.assertIn(value, ALLOWED_ENGLISH_VALUES, f"unreviewed English value in {filename}:{key}")

    def test_key_cjk_ui_terms_remain_unambiguous(self):
        expected = {
            "chinese_simplified.yaml": {
                "STR_EXT_READER_FONT": "阅读字体",
                "STR_EXT_UI_FONT": "界面字体",
                "STR_FIRST_LINE_INDENT": "首行缩进",
                "STR_SECTION_PREFIX": "分节 ",
            },
            "chinese_traditional.yaml": {
                "STR_EXT_READER_FONT": "閱讀字體",
                "STR_EXT_UI_FONT": "介面字體",
                "STR_FIRST_LINE_INDENT": "首行縮排",
                "STR_SECTION_PREFIX": "分節 ",
            },
            "japanese.yaml": {
                "STR_EXT_READER_FONT": "本文フォント",
                "STR_EXT_UI_FONT": "UIフォント",
                "STR_FIRST_LINE_INDENT": "段落先頭の字下げ",
                "STR_FONT_MANIFEST_INVALID": "フォント一覧が無効です",
            },
        }
        for filename, terms in expected.items():
            translated = self.generator.parse_yaml_file(str(TRANSLATIONS / filename))
            with self.subTest(language=filename):
                for key, value in terms.items():
                    self.assertEqual(translated[key], value)

    def test_help_does_not_regenerate_checked_in_files(self):
        before = {path: path.read_bytes() for path in GENERATED_FILES}
        result = subprocess.run(
            [sys.executable, str(GENERATOR), "--help"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("usage:", result.stdout.lower())
        self.assertEqual(before, {path: path.read_bytes() for path in GENERATED_FILES})


if __name__ == "__main__":
    unittest.main()
