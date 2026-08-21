#!/usr/bin/env python3

import pathlib
import shutil
import subprocess
import tempfile


root = pathlib.Path(__file__).resolve().parents[1]
source = root / "test" / "text_fallback_scale" / "TextFallbackScaleTest.cpp"
compiler = next((path for name in ("clang++", "g++", "c++") if (path := shutil.which(name))), None)

if compiler is None:
    raise SystemExit("No host C++ compiler found; run the PlatformIO firmware build instead")

with tempfile.TemporaryDirectory() as temp_dir:
    binary = pathlib.Path(temp_dir) / "TextFallbackScaleTest.exe"
    subprocess.run(
        [
            compiler,
            "-std=c++20",
            "-O2",
            "-Wall",
            "-Wextra",
            "-pedantic",
            f"-I{root}",
            str(source),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

print("TEXT_FALLBACK_SCALE_OK")
