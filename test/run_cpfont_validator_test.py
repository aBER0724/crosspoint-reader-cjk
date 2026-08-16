#!/usr/bin/env python3

import subprocess
import os
import shlex
import shutil
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
TEST_FILE = REPO_ROOT / "test" / "CpfontValidatorTest.cpp"
VALIDATOR_CPP = REPO_ROOT / "src" / "CpfontValidator.cpp"
SRC_INCLUDE = REPO_ROOT / "src"


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


def compile_test(output: Path) -> subprocess.CompletedProcess[str]:
    command = find_compiler() + [
        "-std=c++20",
        f"-I{SRC_INCLUDE}",
        str(TEST_FILE),
        str(VALIDATOR_CPP),
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
    with tempfile.TemporaryDirectory(prefix="cpfont-validator-") as temp_dir_str:
        binary = Path(temp_dir_str) / "CpfontValidatorTest"

        result = compile_test(binary)
        print_result(result)
        if result.returncode != 0:
            return result.returncode

        run_result = run_binary(binary)
        print_result(run_result)
        return run_result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
