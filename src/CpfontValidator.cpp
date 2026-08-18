#include "CpfontValidator.h"

#include <cstdint>
#include <cstring>
#include <limits>

namespace CpfontValidator {
namespace {

constexpr size_t kHeaderSize = 32;
constexpr size_t kStyleTocEntrySize = 32;
constexpr uint16_t kVersion = 4;
constexpr uint16_t kFlags = 1;
constexpr uint8_t kMaxStyles = 4;
constexpr uint32_t kMaxGlyphs = 65536;
constexpr uint32_t kMaxIntervals = kMaxGlyphs;
constexpr uint32_t kMaxKernEntries = 4096;
constexpr uint32_t kUnicodeMax = 0x10FFFF;
constexpr uint32_t kMaxEncodedOffset = std::numeric_limits<uint32_t>::max();
constexpr size_t kReadCacheSize = 256;

struct StyleSummary {
  uint8_t styleId = 0;
  uint32_t intervalCount = 0;
  uint32_t glyphCount = 0;
  uint16_t kernLeftEntryCount = 0;
  uint16_t kernRightEntryCount = 0;
  uint8_t kernLeftClassCount = 0;
  uint8_t kernRightClassCount = 0;
  uint8_t ligatureCount = 0;
  uint32_t dataOffset = 0;
};

uint16_t readU16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1] << 8);
}

uint32_t readU32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

bool checkedAdd(uint64_t left, uint64_t right, uint64_t* result) {
  if (left > std::numeric_limits<uint64_t>::max() - right) return false;
  *result = left + right;
  return true;
}

bool checkedMultiply(uint64_t left, uint64_t right, uint64_t* result) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) return false;
  *result = left * right;
  return true;
}

bool canRead(const Reader& reader, uint64_t offset, uint64_t length) {
  return offset <= reader.fileSize && length <= static_cast<uint64_t>(reader.fileSize) - offset &&
         offset <= std::numeric_limits<size_t>::max() && length <= std::numeric_limits<size_t>::max();
}

class BufferedReader {
 public:
  explicit BufferedReader(const Reader& reader) : reader_(reader) {}

  bool read(uint64_t offset, void* destination, size_t length) {
    if (!canRead(reader_, offset, length)) return false;
    if (length > sizeof(cache_)) return readDirect(offset, destination, length);

    uint64_t end = 0;
    if (!checkedAdd(offset, length, &end)) return false;
    const uint64_t cacheEnd = cacheOffset_ + cacheLength_;
    if (cacheLength_ == 0 || offset < cacheOffset_ || end > cacheEnd) {
      cacheOffset_ = offset;
      cacheLength_ = static_cast<size_t>((static_cast<uint64_t>(reader_.fileSize) - offset < sizeof(cache_))
                                             ? static_cast<uint64_t>(reader_.fileSize) - offset
                                             : sizeof(cache_));
      if (!readDirect(cacheOffset_, cache_, cacheLength_)) {
        cacheLength_ = 0;
        return false;
      }
    }

    std::memcpy(destination, cache_ + static_cast<size_t>(offset - cacheOffset_), length);
    return true;
  }

 private:
  bool readDirect(uint64_t offset, void* destination, size_t length) {
    return reader_.readAt != nullptr && canRead(reader_, offset, length) &&
           reader_.readAt(reader_.context, static_cast<size_t>(offset), destination, length) ==
               static_cast<int>(length);
  }

  const Reader& reader_;
  uint8_t cache_[kReadCacheSize] = {};
  uint64_t cacheOffset_ = 0;
  size_t cacheLength_ = 0;
};

bool isAllZero(const uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] != 0) return false;
  }
  return true;
}

bool validateIntervals(BufferedReader& reader, const StyleSummary& style, uint64_t sectionOffset) {
  uint64_t glyphOffset = 0;
  uint32_t previousLast = 0;
  uint8_t interval[12];

  for (uint32_t i = 0; i < style.intervalCount; ++i) {
    uint64_t offset = 0;
    if (!checkedMultiply(i, sizeof(interval), &offset) || !checkedAdd(sectionOffset, offset, &offset) ||
        !reader.read(offset, interval, sizeof(interval))) {
      return false;
    }

    const uint32_t first = readU32(interval);
    const uint32_t last = readU32(interval + 4);
    const uint32_t offsetIntoGlyphs = readU32(interval + 8);
    if (first > last || last > kUnicodeMax || (i > 0 && first <= previousLast)) return false;

    const uint64_t span = static_cast<uint64_t>(last) - first + 1;
    if (span > style.glyphCount || offsetIntoGlyphs != glyphOffset ||
        static_cast<uint64_t>(offsetIntoGlyphs) > style.glyphCount - span) {
      return false;
    }
    glyphOffset += span;
    previousLast = last;
  }

  return glyphOffset == style.glyphCount;
}

bool validateGlyphs(BufferedReader& reader, const StyleSummary& style, uint64_t sectionOffset, uint64_t bitmapSize) {
  uint64_t runningBitmapOffset = 0;
  uint8_t glyph[16];

  for (uint32_t i = 0; i < style.glyphCount; ++i) {
    uint64_t offset = 0;
    if (!checkedMultiply(i, sizeof(glyph), &offset) || !checkedAdd(sectionOffset, offset, &offset) ||
        !reader.read(offset, glyph, sizeof(glyph))) {
      return false;
    }

    const uint8_t width = glyph[0];
    const uint8_t height = glyph[1];
    const uint16_t dataLength = readU16(glyph + 8);
    const uint32_t dataOffset = readU32(glyph + 12);
    const uint64_t area = static_cast<uint64_t>(width) * height;
    const uint64_t expectedLength = (area + 3) / 4;
    if (!isAllZero(glyph + 10, 2) || expectedLength != dataLength || dataOffset != runningBitmapOffset ||
        static_cast<uint64_t>(dataOffset) > bitmapSize || static_cast<uint64_t>(dataLength) > bitmapSize - dataOffset) {
      return false;
    }

    runningBitmapOffset += dataLength;
  }

  return runningBitmapOffset == bitmapSize;
}

bool validateKernEntries(BufferedReader& reader, uint64_t sectionOffset, uint16_t entryCount, uint8_t classCount) {
  bool seenClasses[256] = {};
  uint16_t previousCodepoint = 0;
  uint8_t entry[3];

  for (uint16_t i = 0; i < entryCount; ++i) {
    uint64_t offset = 0;
    if (!checkedMultiply(i, sizeof(entry), &offset) || !checkedAdd(sectionOffset, offset, &offset) ||
        !reader.read(offset, entry, sizeof(entry))) {
      return false;
    }

    const uint16_t codepoint = readU16(entry);
    const uint8_t classId = entry[2];
    if ((i > 0 && codepoint <= previousCodepoint) || classId == 0 || classId > classCount) return false;
    seenClasses[classId] = true;
    previousCodepoint = codepoint;
  }

  for (uint16_t classId = 1; classId <= classCount; ++classId) {
    if (!seenClasses[classId]) return false;
  }
  return true;
}

bool validateLigatures(BufferedReader& reader, uint64_t sectionOffset, uint8_t ligatureCount) {
  uint32_t previousPair = 0;
  uint8_t entry[8];

  for (uint8_t i = 0; i < ligatureCount; ++i) {
    uint64_t offset = 0;
    if (!checkedMultiply(i, sizeof(entry), &offset) || !checkedAdd(sectionOffset, offset, &offset) ||
        !reader.read(offset, entry, sizeof(entry))) {
      return false;
    }

    const uint32_t pair = readU32(entry);
    const uint32_t replacement = readU32(entry + 4);
    if ((i > 0 && pair <= previousPair) || replacement > kUnicodeMax) return false;
    previousPair = pair;
  }
  return true;
}

bool validateStyle(BufferedReader& reader, const StyleSummary& style, uint64_t nextOffset, uint64_t fileSize,
                   uint64_t expectedDataOffset) {
  if (style.dataOffset != expectedDataOffset || nextOffset < style.dataOffset || nextOffset > fileSize) return false;

  uint64_t sectionOffset = style.dataOffset;
  uint64_t sectionSize = 0;
  uint64_t sectionEnd = 0;

  if (!checkedMultiply(style.intervalCount, 12, &sectionSize) || !checkedAdd(sectionOffset, sectionSize, &sectionEnd) ||
      sectionEnd > nextOffset || !validateIntervals(reader, style, sectionOffset)) {
    return false;
  }
  sectionOffset = sectionEnd;

  if (!checkedMultiply(style.glyphCount, 16, &sectionSize) || !checkedAdd(sectionOffset, sectionSize, &sectionEnd) ||
      sectionEnd > nextOffset) {
    return false;
  }
  const uint64_t glyphSectionOffset = sectionOffset;
  sectionOffset = sectionEnd;

  const bool noKern = style.kernLeftEntryCount == 0 && style.kernRightEntryCount == 0 &&
                      style.kernLeftClassCount == 0 && style.kernRightClassCount == 0;
  const bool hasKern = style.kernLeftEntryCount > 0 && style.kernRightEntryCount > 0 && style.kernLeftClassCount > 0 &&
                       style.kernRightClassCount > 0;
  if (!noKern && !hasKern) return false;
  if (!checkedMultiply(style.kernLeftEntryCount, 3, &sectionSize) ||
      !checkedAdd(sectionOffset, sectionSize, &sectionEnd) || sectionEnd > nextOffset ||
      (hasKern && !validateKernEntries(reader, sectionOffset, style.kernLeftEntryCount, style.kernLeftClassCount))) {
    return false;
  }
  sectionOffset = sectionEnd;

  if (!checkedMultiply(style.kernRightEntryCount, 3, &sectionSize) ||
      !checkedAdd(sectionOffset, sectionSize, &sectionEnd) || sectionEnd > nextOffset ||
      (hasKern && !validateKernEntries(reader, sectionOffset, style.kernRightEntryCount, style.kernRightClassCount))) {
    return false;
  }
  sectionOffset = sectionEnd;

  uint64_t matrixSize = 0;
  if (!checkedMultiply(style.kernLeftClassCount, style.kernRightClassCount, &matrixSize) ||
      !checkedAdd(sectionOffset, matrixSize, &sectionEnd) || sectionEnd > nextOffset) {
    return false;
  }
  sectionOffset = sectionEnd;

  if (!checkedMultiply(style.ligatureCount, 8, &sectionSize) || !checkedAdd(sectionOffset, sectionSize, &sectionEnd) ||
      sectionEnd > nextOffset || !validateLigatures(reader, sectionOffset, style.ligatureCount)) {
    return false;
  }
  sectionOffset = sectionEnd;

  const uint64_t bitmapSize = nextOffset - sectionOffset;
  if (!validateGlyphs(reader, style, glyphSectionOffset, bitmapSize)) return false;
  return true;
}

}  // namespace

bool validateV4(const Reader& reader) {
  if (reader.readAt == nullptr || reader.fileSize < kHeaderSize ||
      static_cast<uint64_t>(reader.fileSize) > kMaxEncodedOffset) {
    return false;
  }

  BufferedReader bufferedReader(reader);
  uint8_t header[kHeaderSize];
  if (!bufferedReader.read(0, header, sizeof(header))) return false;
  constexpr uint8_t kMagic[] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
  if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0 || readU16(header + 8) != kVersion ||
      readU16(header + 10) != kFlags || header[12] == 0 || header[12] > kMaxStyles || !isAllZero(header + 13, 19)) {
    return false;
  }

  const uint8_t styleCount = header[12];
  uint64_t tocEnd = 0;
  if (!checkedMultiply(styleCount, kStyleTocEntrySize, &tocEnd) || !checkedAdd(kHeaderSize, tocEnd, &tocEnd) ||
      tocEnd > reader.fileSize) {
    return false;
  }

  StyleSummary styles[kMaxStyles];
  uint8_t toc[kStyleTocEntrySize];
  for (uint8_t i = 0; i < styleCount; ++i) {
    const uint64_t tocOffset = kHeaderSize + static_cast<uint64_t>(i) * kStyleTocEntrySize;
    if (!bufferedReader.read(tocOffset, toc, sizeof(toc)) || !isAllZero(toc + 1, 3) || !isAllZero(toc + 28, 4)) {
      return false;
    }

    StyleSummary& style = styles[i];
    style.styleId = toc[0];
    style.intervalCount = readU32(toc + 4);
    style.glyphCount = readU32(toc + 8);
    style.kernLeftEntryCount = readU16(toc + 17);
    style.kernRightEntryCount = readU16(toc + 19);
    style.kernLeftClassCount = toc[21];
    style.kernRightClassCount = toc[22];
    style.ligatureCount = toc[23];
    style.dataOffset = readU32(toc + 24);

    if (style.styleId >= kMaxStyles || (i > 0 && style.styleId <= styles[i - 1].styleId) ||
        style.intervalCount > kMaxIntervals || style.glyphCount > kMaxGlyphs ||
        style.intervalCount > style.glyphCount || style.kernLeftEntryCount > kMaxKernEntries ||
        style.kernRightEntryCount > kMaxKernEntries) {
      return false;
    }
  }

  uint64_t expectedDataOffset = tocEnd;
  for (uint8_t i = 0; i < styleCount; ++i) {
    const uint64_t nextOffset = (i + 1 < styleCount) ? styles[i + 1].dataOffset : reader.fileSize;
    if (!validateStyle(bufferedReader, styles[i], nextOffset, reader.fileSize, expectedDataOffset)) return false;
    expectedDataOffset = nextOffset;
  }
  return expectedDataOffset == reader.fileSize;
}

}  // namespace CpfontValidator
