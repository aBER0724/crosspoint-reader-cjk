#pragma once

#include <cstdint>

namespace EpubSectionCache {

inline constexpr uint8_t kSectionFileVersion = 25;
inline constexpr uint32_t kSectionHeaderSize = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) +
                                               sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) +
                                               sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) +
                                               sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

struct Parameters {
  int fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool firstLineIndent = false;
  bool embeddedStyle = false;
  uint8_t imageRendering = 0;
};

inline bool parametersMatch(const Parameters& current, const Parameters& cached) {
  return current.fontId == cached.fontId && current.lineCompression == cached.lineCompression &&
         current.extraParagraphSpacing == cached.extraParagraphSpacing &&
         current.paragraphAlignment == cached.paragraphAlignment && current.viewportWidth == cached.viewportWidth &&
         current.viewportHeight == cached.viewportHeight && current.hyphenationEnabled == cached.hyphenationEnabled &&
         current.firstLineIndent == cached.firstLineIndent && current.embeddedStyle == cached.embeddedStyle &&
         current.imageRendering == cached.imageRendering;
}

}  // namespace EpubSectionCache
