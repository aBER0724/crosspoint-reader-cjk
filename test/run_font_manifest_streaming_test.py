#!/usr/bin/env python3

import json
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST = Path("/tmp/fonts.json")
SOURCE = REPO_ROOT / "src" / "activities" / "settings" / "FontDownloadActivity.cpp"


class StreamingFontManifestTest(unittest.TestCase):
    def test_production_manifest_families_fit_bounded_parser(self):
        self.assertTrue(MANIFEST.exists(), "download the production fonts.json to /tmp/fonts.json")
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.assertGreater(len(manifest["families"]), 40)
        largest_family = max(len(json.dumps(family, separators=(",", ":"))) for family in manifest["families"])
        self.assertLess(largest_family, 2048)

    def test_firmware_does_not_deserialize_the_whole_manifest(self):
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn("parseNextManifestFamily", source)
        self.assertNotIn("deserializeJson(doc, manifestFile, DeserializationOption::Filter(filter))", source)


if __name__ == "__main__":
    unittest.main()
