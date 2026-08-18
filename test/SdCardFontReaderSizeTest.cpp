#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "SdCardFontRegistry.h"

namespace {

SdCardFontFamilyInfo makeFamily(const std::vector<uint8_t>& sizes, const uint8_t style = 0) {
  SdCardFontFamilyInfo family;
  family.name = "TestFamily";
  for (const uint8_t size : sizes) {
    family.files.push_back({"/fonts/TestFamily/TestFamily_" + std::to_string(size) + ".cpfont", size, style});
  }
  return family;
}

void expectSlots(const char* name, const SdCardFontFamilyInfo& family, const std::array<uint8_t, 4>& expected,
                 const uint8_t style = 0) {
  for (uint8_t slot = 0; slot < expected.size(); ++slot) {
    const SdCardFontFileInfo* file = family.findClosestReaderSize(slot, style);
    if (!file || file->pointSize != expected[slot]) {
      std::cerr << name << ": slot " << static_cast<int>(slot) << " expected " << static_cast<int>(expected[slot])
                << " pt, got " << (file ? std::to_string(file->pointSize) : "null") << "\n";
      std::exit(1);
    }
  }
}

}  // namespace

int main() {
  expectSlots("new catalog excludes UI sizes", makeFamily({8, 10, 12, 14, 16, 18, 22}), {14, 16, 18, 22});
  expectSlots("new reader-only catalog", makeFamily({14, 16, 18, 22}), {14, 16, 18, 22});
  expectSlots("legacy catalog remains compatible", makeFamily({8, 10, 12, 14, 16, 18}), {12, 14, 16, 18});
  expectSlots("legacy reader-only catalog", makeFamily({12, 14, 16, 18}), {12, 14, 16, 18});
  expectSlots("custom catalog ignores UI files", makeFamily({8, 10, 12, 13, 15, 17, 20}), {13, 15, 17, 20});
  expectSlots("reader-only custom catalog keeps 12 pt", makeFamily({12, 13, 15, 17}), {12, 13, 15, 17});
  expectSlots("sparse catalog uses reader targets", makeFamily({8, 10, 12, 16, 22}), {16, 16, 16, 22});

  const auto styled = makeFamily({14, 16, 18, 22}, 1);
  expectSlots("style filtering", styled, {14, 16, 18, 22}, 1);
  if (styled.findClosestReaderSize(0, 0) != nullptr) {
    std::cerr << "style filtering: missing style should return null\n";
    return 1;
  }

  const auto clamped = makeFamily({14, 16, 18, 22});
  const SdCardFontFileInfo* clampedFile = clamped.findClosestReaderSize(255);
  if (!clampedFile || clampedFile->pointSize != 22) {
    std::cerr << "invalid persisted slot should clamp to 22 pt\n";
    return 1;
  }

  std::cout << "SdCardFont reader-size mapping tests passed.\n";
  return 0;
}
