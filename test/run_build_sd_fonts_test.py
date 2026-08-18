#!/usr/bin/env python3

import importlib.util
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = REPO_ROOT / "lib" / "EpdFont" / "scripts" / "build-sd-fonts.py"


def load_build_module():
    spec = importlib.util.spec_from_file_location("build_sd_fonts", BUILD_SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load {BUILD_SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ArchiveMemberExtractionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_build_module()

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory(prefix="build-sd-fonts-")
        self.root = Path(self.temp_dir.name)
        self.download_dir = self.root / "downloads"
        self.download_patch = mock.patch.object(self.module, "DOWNLOAD_DIR", self.download_dir)
        self.download_patch.start()

    def tearDown(self):
        self.download_patch.stop()
        self.temp_dir.cleanup()

    def create_archive(self, name: str = "fonts.zip", member: str = "fonts/Test.ttf") -> Path:
        archive_path = self.root / name
        with zipfile.ZipFile(archive_path, "w") as archive:
            archive.writestr(member, b"test-font-data")
        return archive_path

    def test_extracts_named_font_member_and_reuses_cache(self):
        archive_path = self.create_archive()

        extracted = self.module.extract_archive_member(archive_path, "fonts/Test.ttf", "TestFamily", "regular")
        self.assertEqual(extracted.read_bytes(), b"test-font-data")

        extracted.write_bytes(b"cached-font-data")
        cached = self.module.extract_archive_member(archive_path, "fonts/Test.ttf", "TestFamily", "regular")
        self.assertEqual(cached, extracted)
        self.assertEqual(cached.read_bytes(), b"cached-font-data")

    def test_rejects_non_font_members(self):
        archive_path = self.create_archive(member="README.txt")

        with self.assertRaisesRegex(ValueError, "must be a TTF or OTF"):
            self.module.extract_archive_member(archive_path, "README.txt", "TestFamily", "regular")

    def test_reports_missing_or_invalid_archives(self):
        archive_path = self.create_archive()
        with self.assertRaisesRegex(RuntimeError, "cannot extract missing.ttf"):
            self.module.extract_archive_member(archive_path, "missing.ttf", "TestFamily", "regular")

        invalid_archive = self.root / "invalid.zip"
        invalid_archive.write_bytes(b"not-a-zip")
        with self.assertRaisesRegex(RuntimeError, "cannot extract Test.ttf"):
            self.module.extract_archive_member(invalid_archive, "Test.ttf", "TestFamily", "regular")


if __name__ == "__main__":
    unittest.main()
