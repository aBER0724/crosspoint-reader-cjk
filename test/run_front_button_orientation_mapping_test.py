#!/usr/bin/env python3
"""Compile and run the front-button rotation policy contract."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SOURCE = r'''
#include <cassert>
#include <cstdint>

#include "input/FrontButtonOrientation.h"

using FrontButtonOrientation::Orientation;
using FrontButtonOrientation::hintHardwareIndex;
using FrontButtonOrientation::inputHardwareIndex;
using FrontButtonOrientation::navigationSwapped;

int main() {
  for (uint8_t button = 0; button < 4; ++button) {
    // Upright and landscape modes keep configured front roles on their raw targets.
    assert(inputHardwareIndex(false, Orientation::Portrait, button) == button);
    assert(inputHardwareIndex(true, Orientation::Portrait, button) == button);
    assert(inputHardwareIndex(true, Orientation::LandscapeClockwise, button) == button);
    assert(inputHardwareIndex(true, Orientation::LandscapeCounterClockwise, button) == button);

    // In inverted portrait, following orientation mirrors all four configured roles.
    assert(inputHardwareIndex(false, Orientation::PortraitInverted, button) == button);
    assert(inputHardwareIndex(true, Orientation::PortraitInverted, button) == 3 - button);

    // With following disabled, hints must describe the unchanged raw roles at their rotated positions.
    assert(hintHardwareIndex(false, Orientation::PortraitInverted, button) == 3 - button);
    assert(hintHardwareIndex(true, Orientation::PortraitInverted, button) == button);
    assert(hintHardwareIndex(true, Orientation::Portrait, button) == button);
  }

  // Mirroring must not underflow if persisted settings are corrupt.
  assert(inputHardwareIndex(true, Orientation::PortraitInverted, 4) == 4);
  assert(hintHardwareIndex(false, Orientation::PortraitInverted, 255) == 255);

  // Portrait inversion is handled by front-role mirroring, so list semantics stay in the
  // same on-screen order and must not receive a second previous/next swap.
  assert(!navigationSwapped(false, Orientation::PortraitInverted));
  assert(!navigationSwapped(true, Orientation::Portrait));
  assert(!navigationSwapped(true, Orientation::PortraitInverted));
  assert(!navigationSwapped(true, Orientation::LandscapeClockwise));
  assert(navigationSwapped(true, Orientation::LandscapeCounterClockwise));
}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp_path = Path(tmp)
    source = tmp_path / "front_button_orientation_test.cpp"
    binary = tmp_path / "front_button_orientation_test"
    source.write_text(SOURCE)
    subprocess.run(
        ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Isrc", str(source), "-o", str(binary)],
        cwd=ROOT,
        check=True,
    )
    subprocess.run([str(binary)], check=True)

print("FRONT_BUTTON_ORIENTATION_MAPPING_OK")
