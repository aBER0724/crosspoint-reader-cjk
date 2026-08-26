#!/usr/bin/env python3
"""Generate size- and weight-matched built-in Noto Sans CJK UI fonts."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys


FONT_CONFIGS = (
    (8, 23, 18, "regular", Path("fonts/NotoSansCJKSC-UI-Regular.otf")),
    (8, 23, 18, "bold", Path("fonts/NotoSansCJKSC-UI-Bold.otf")),
    (10, 28, 23, "regular", Path("fonts/NotoSansCJKSC-UI-Regular.otf")),
    (10, 28, 23, "bold", Path("fonts/NotoSansCJKSC-UI-Bold.otf")),
    (12, 34, 27, "regular", Path("fonts/NotoSansCJKSC-UI-Regular.otf")),
    (12, 34, 27, "bold", Path("fonts/NotoSansCJKSC-UI-Bold.otf")),
)
GENERATOR_PATH = Path("scripts/generate_cjk_ui_font.py")
TRANSLATIONS_PATH = Path("lib/I18n/translations")
DEFAULT_LANGUAGE_FILTER = ["ENGLISH", "CHINESE_SIMPLIFIED", "CHINESE_TRADITIONAL", "JAPANESE"]


def split_language_filter(raw: str) -> list[str]:
    return [lang.strip() for lang in raw.split(",") if lang.strip()]


def generate_builtin_cjk_fonts(language_filter: list[str] | None = None) -> None:
    project_root = Path(__file__).resolve().parent.parent
    generator_path = project_root / GENERATOR_PATH
    translations_path = project_root / TRANSLATIONS_PATH

    for size, cell_size, baseline, style, relative_font_path in FONT_CONFIGS:
        font_path = project_root / relative_font_path
        output_path = project_root / f"lib/GfxRenderer/cjk_ui_font_{size}_{style}.h"
        if not font_path.is_file():
            print(f"Error: built-in CJK font source not found: {font_path}")
            sys.exit(1)

        cmd = [
            sys.executable,
            str(generator_path),
            "--size",
            str(size),
            "--cell-size",
            str(cell_size),
            "--baseline",
            str(baseline),
            "--font",
            str(font_path),
            "--output",
            str(output_path),
            "--namespace",
            f"CjkUiFont{size}{style.title()}",
            "--translations-dir",
            str(translations_path),
        ]
        if language_filter:
            cmd.extend(["--languages", ",".join(language_filter)])

        print(f"Generating built-in Noto Sans CJK UI font: {size}pt/{cell_size}px {style}", flush=True)
        result = subprocess.run(cmd, check=False)
        if result.returncode != 0:
            sys.exit(result.returncode)


def main() -> None:
    generate_builtin_cjk_fonts(DEFAULT_LANGUAGE_FILTER)


if __name__ == "__main__":
    main()
else:
    try:
        Import("env")
        _language_filter = None
        try:
            _languages = env.GetProjectOption("custom_i18n_languages", "")
            if _languages:
                _language_filter = split_language_filter(_languages)
        except Exception:
            pass
        generate_builtin_cjk_fonts(_language_filter)
    except NameError:
        pass
