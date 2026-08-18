#!/usr/bin/env python3

import importlib.util
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_SCRIPT = REPO_ROOT / "scripts" / "generate-font-manifest.py"
FONT_CONFIG = REPO_ROOT / "lib" / "EpdFont" / "scripts" / "sd-fonts.yaml"


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


if __name__ == "__main__":
    unittest.main()
