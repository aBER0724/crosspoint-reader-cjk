#!/usr/bin/env python3
"""Validate release workflow references against PlatformIO and artifact rules."""

from __future__ import annotations

import configparser
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW_DIR = ROOT / ".github" / "workflows"
PLATFORMIO = ROOT / "platformio.ini"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def platformio_envs() -> set[str]:
    parser = configparser.RawConfigParser(strict=False)
    parser.read(PLATFORMIO, encoding="utf-8")
    return {
        section.removeprefix("env:")
        for section in parser.sections()
        if section.startswith("env:")
    }


def check_platformio_env_references(failures: list[str]) -> None:
    envs = platformio_envs()
    workflow_refs: list[tuple[Path, str]] = []
    build_path_refs: list[tuple[Path, str]] = []

    for path in sorted(WORKFLOW_DIR.glob("*.yml")):
        text = read(path)
        for match in re.finditer(r"\bpio\s+run\b[^\n]*?\s-e\s+([A-Za-z0-9_]+)\b", text):
            workflow_refs.append((path, match.group(1)))
        for match in re.finditer(r"\.pio/build/([A-Za-z0-9_]+)/", text):
            build_path_refs.append((path, match.group(1)))

    for path, env in workflow_refs + build_path_refs:
        if env not in envs:
            failures.append(f"{path.relative_to(ROOT)} references undefined PlatformIO env {env!r}")


def check_setup_uv_pinned(failures: list[str]) -> None:
    for path in sorted(WORKFLOW_DIR.glob("*.yml")):
        lines = read(path).splitlines()
        for index, line in enumerate(lines):
            if "astral-sh/setup-uv@" not in line:
                continue
            block = "\n".join(lines[index : index + 8])
            version = re.search(r"\bversion:\s*[\"']?([^\"'\s]+)", block)
            if not version:
                failures.append(f"{path.relative_to(ROOT)} uses setup-uv without an explicit uv version")
            elif version.group(1).lower() == "latest":
                failures.append(f"{path.relative_to(ROOT)} uses setup-uv with version: latest")


def check_release_checksums(failures: list[str]) -> None:
    release_workflows = [
        WORKFLOW_DIR / "release.yml",
        WORKFLOW_DIR / "release_candidate.yml",
        WORKFLOW_DIR / "sd_recovery.yml",
    ]
    for path in release_workflows:
        text = read(path)
        if "sha256sum" not in text:
            failures.append(f"{path.relative_to(ROOT)} does not generate SHA256SUMS")
        if "SHA256SUMS" not in text:
            failures.append(f"{path.relative_to(ROOT)} does not publish SHA256SUMS")


def main() -> int:
    failures: list[str] = []
    check_platformio_env_references(failures)
    check_setup_uv_pinned(failures)
    check_release_checksums(failures)

    if failures:
        print("Release contract validation failed:")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print("Release contract validation passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
