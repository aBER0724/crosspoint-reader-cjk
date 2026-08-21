#!/usr/bin/env python3

import subprocess
import os
import shlex
import shutil
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
TEST_FILE = REPO_ROOT / "test" / "SdCardFontReaderSizeTest.cpp"
REGISTRY_HEADER = REPO_ROOT / "lib" / "EpdFont" / "SdCardFontRegistry.h"


def find_compiler() -> list[str]:
    if compiler := os.environ.get("CXX"):
        return shlex.split(compiler)

    for compiler in ("g++", "clang++", "c++"):
        if executable := shutil.which(compiler):
            return [executable]

    if executable := shutil.which("zig"):
        return [executable, "c++"]

    zig_caches = sorted(Path(tempfile.gettempdir()).glob("codex-ziglang-*/ziglang/zig.exe"), reverse=True)
    if zig_caches:
        return [str(zig_caches[0]), "c++"]

    raise RuntimeError("No host C++ compiler found; set CXX or install g++, clang++, or Zig")


def generate_helpers(output: Path) -> None:
    source = (REPO_ROOT / "lib" / "EpdFont" / "SdCardFontRegistry.cpp").read_text(encoding="utf-8")
    marker = "// --- SdCardFontRegistry ---"
    if marker not in source:
        raise RuntimeError("Cannot locate SdCardFontFamilyInfo helper boundary")
    helper_body = source[source.index("const SdCardFontFileInfo*") : source.index(marker)]
    output.write_text('#include "SdCardFontRegistry.h"\n\n' + helper_body, encoding="utf-8")


def compile_test(output: Path, helpers: Path) -> subprocess.CompletedProcess[str]:
    command = find_compiler() + [
        "-std=c++20",
        f"-I{REGISTRY_HEADER.parent}",
        str(TEST_FILE),
        str(helpers),
        "-o",
        str(output),
    ]
    return subprocess.run(command, text=True, capture_output=True, cwd=REPO_ROOT)


def run_binary(binary: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(binary)], text=True, capture_output=True, cwd=REPO_ROOT)


def print_result(result: subprocess.CompletedProcess[str]) -> None:
    if result.stdout:
        sys.stdout.write(result.stdout)
    if result.stderr:
        sys.stderr.write(result.stderr)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="sd-font-reader-size-") as temp_dir_str:
        temp_dir = Path(temp_dir_str)
        helpers = temp_dir / "SdCardFontFamilyInfo.cpp"
        binary = temp_dir / "SdCardFontReaderSizeTest"
        generate_helpers(helpers)

        result = compile_test(binary, helpers)
        print_result(result)
        if result.returncode != 0:
            return result.returncode

        run_result = run_binary(binary)
        print_result(run_result)
        return run_result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
