#!/usr/bin/env python3
"""Ensure interactive EPUB turns batch-prewarm SD-card glyphs."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "activities" / "reader" / "EpubReaderActivity.cpp"


def main() -> int:
    content = SOURCE.read_text(encoding="utf-8")
    required = [
        "if (renderer.isSdCardFont(fontId)) {",
        "prewarmScope.emplace(fcm->createPrewarmScope());",
        "prewarmScope->endScanAndPrewarm(cancellation.isCancelled, cancellation.context)",
        "SD-font page turns still batch-prewarm",
    ]
    forbidden = [
        "if (!interactiveRender && renderer.isSdCardFont(fontId)) {",
        "interactive renders load\n  // missing glyphs on demand",
    ]
    failures = []
    for needle in required:
        if needle not in content:
            failures.append(f"missing required snippet: {needle}")
    for needle in forbidden:
        if needle in content:
            failures.append(f"forbidden slow path remains: {needle}")
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print("EPUB_SD_FONT_INTERACTIVE_PREWARM_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
