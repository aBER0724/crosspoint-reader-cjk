#!/usr/bin/env python3

import hashlib
import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_SCRIPT = REPO_ROOT / "scripts" / "generate-font-manifest.py"
FONT_CONFIG = REPO_ROOT / "lib" / "EpdFont" / "scripts" / "sd-fonts.yaml"
FONT_DOWNLOAD_HEADER = REPO_ROOT / "src" / "activities" / "settings" / "FontDownloadActivity.h"
FONT_DOWNLOAD_SOURCE = REPO_ROOT / "src" / "activities" / "settings" / "FontDownloadActivity.cpp"


def load_manifest_module():
    spec = importlib.util.spec_from_file_location("generate_font_manifest", MANIFEST_SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load {MANIFEST_SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ManifestMetadataTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_manifest_module()

    def test_loads_font_provenance_from_catalog(self):
        metadata = self.module.load_metadata_from_yaml(FONT_CONFIG)

        for family_name in ("IansuiTC", "GenWanSerifTC", "GenYoGothicTC"):
            family = metadata[family_name]
            self.assertEqual(family["license"], "OFL-1.1")
            self.assertTrue(family["licenseUrl"].startswith("https://github.com/ButTaiwan/"))
            self.assertTrue(family["sourceUrl"].startswith("https://github.com/ButTaiwan/"))

    def test_manifest_includes_optional_provenance(self):
        with tempfile.TemporaryDirectory(prefix="font-manifest-") as temp_dir:
            font_path = Path(temp_dir) / "IansuiTC_12.cpfont"
            font_path.write_bytes(b"test")
            original_metadata = self.module.FAMILY_METADATA
            original_style_reader = self.module.read_cpfont_styles
            try:
                self.module.FAMILY_METADATA = self.module.load_metadata_from_yaml(FONT_CONFIG)
                self.module.read_cpfont_styles = lambda _: ["regular"]
                manifest = self.module.build_manifest({"IansuiTC": [font_path]}, "https://example.test/")
            finally:
                self.module.FAMILY_METADATA = original_metadata
                self.module.read_cpfont_styles = original_style_reader

        family = manifest["families"][0]
        self.assertEqual(family["license"], "OFL-1.1")
        self.assertIn("licenseUrl", family)
        self.assertIn("sourceUrl", family)

    def test_manifest_v2_uses_exact_sha256_digest(self):
        payload = b"crosspoint-font-manifest-v2"
        with tempfile.TemporaryDirectory(prefix="font-manifest-") as temp_dir:
            font_path = Path(temp_dir) / "TestFamily_12.cpfont"
            font_path.write_bytes(payload)
            original_metadata = self.module.FAMILY_METADATA
            original_style_reader = self.module.read_cpfont_styles
            try:
                self.module.FAMILY_METADATA = {"TestFamily": {"description": "Test"}}
                self.module.read_cpfont_styles = lambda _: ["regular"]
                manifest = self.module.build_manifest({"TestFamily": [font_path]}, "https://example.test/")
            finally:
                self.module.FAMILY_METADATA = original_metadata
                self.module.read_cpfont_styles = original_style_reader

        file_entry = manifest["families"][0]["files"][0]
        self.assertEqual(manifest["version"], 2)
        self.assertEqual(file_entry["sha256"], hashlib.sha256(payload).hexdigest())
        self.assertRegex(file_entry["sha256"], r"^[0-9a-f]{64}$")
        self.assertNotIn("crc32", file_entry)

    def test_firmware_requires_canonical_sha256_manifest(self):
        header = FONT_DOWNLOAD_HEADER.read_text(encoding="utf-8")
        source = FONT_DOWNLOAD_SOURCE.read_text(encoding="utf-8")

        version = re.search(r"#define FONTS_MANIFEST_VERSION\s+(\d+)", header)
        self.assertIsNotNone(version)
        self.assertEqual(int(version.group(1)), self.module.FONTS_MANIFEST_VERSION)
        self.assertIn('!fileObj["sha256"].is<const char*>()', source)
        self.assertIn("parseSha256(fileObj[\"sha256\"].as<const char*>(), file.sha256)", source)
        self.assertIn("DeserializationOption::Filter(filter)", source)
        self.assertIn("mbedtls_sha256_update", source)
        self.assertIn("actualSha256 != file.sha256", source)
        self.assertNotIn('fileObj["crc32"]', source)
        self.assertNotIn("computeFileCrc32", source)


if __name__ == "__main__":
    unittest.main()
