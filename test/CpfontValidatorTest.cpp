#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "CpfontValidator.h"

namespace {

constexpr size_t kHeaderSize = 32;
constexpr size_t kTocEntrySize = 32;
constexpr size_t kTocStart = kHeaderSize;
constexpr size_t kTocStyleId = 0;
constexpr size_t kTocIntervalCount = 4;
constexpr size_t kTocGlyphCount = 8;
constexpr size_t kTocKernLeftEntryCount = 17;
constexpr size_t kTocKernRightEntryCount = 19;
constexpr size_t kTocKernLeftClassCount = 21;
constexpr size_t kTocKernRightClassCount = 22;
constexpr size_t kTocLigatureCount = 23;
constexpr size_t kTocDataOffset = 24;

struct Glyph {
  uint8_t width;
  uint8_t height;
  uint16_t advanceX;
  int16_t left;
  int16_t top;
  std::vector<uint8_t> bitmap;
};

struct KernEntry {
  uint16_t codepoint;
  uint8_t classId;
};

struct Ligature {
  uint32_t pair;
  uint32_t replacement;
};

struct Style {
  uint8_t id;
  std::vector<std::pair<uint32_t, uint32_t>> intervals;
  std::vector<Glyph> glyphs;
  uint8_t advanceY = 16;
  int16_t ascender = 12;
  int16_t descender = -4;
  std::vector<KernEntry> leftKern;
  std::vector<KernEntry> rightKern;
  uint8_t leftClassCount = 0;
  uint8_t rightClassCount = 0;
  std::vector<int8_t> kernMatrix;
  std::vector<Ligature> ligatures;
};

struct SectionOffsets {
  size_t intervals;
  size_t glyphs;
  size_t leftKern;
  size_t rightKern;
  size_t matrix;
  size_t ligatures;
  size_t bitmap;
};

void fail(const std::string& message) {
  std::cerr << message << std::endl;
  std::exit(1);
}

void expect(bool condition, const std::string& message) {
  if (!condition) fail(message);
}

void appendU16(std::vector<uint8_t>& data, uint16_t value) {
  data.push_back(static_cast<uint8_t>(value));
  data.push_back(static_cast<uint8_t>(value >> 8));
}

void appendI16(std::vector<uint8_t>& data, int16_t value) { appendU16(data, static_cast<uint16_t>(value)); }

void appendU32(std::vector<uint8_t>& data, uint32_t value) {
  data.push_back(static_cast<uint8_t>(value));
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value >> 16));
  data.push_back(static_cast<uint8_t>(value >> 24));
}

void writeU16(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
  expect(offset + 2 <= data.size(), "writeU16 offset is outside fixture");
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  expect(offset + 4 <= data.size(), "writeU32 offset is outside fixture");
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint32_t readU32(const std::vector<uint8_t>& data, size_t offset) {
  expect(offset + 4 <= data.size(), "readU32 offset is outside fixture");
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void appendInterval(std::vector<uint8_t>& data, uint32_t first, uint32_t last, uint32_t glyphOffset) {
  appendU32(data, first);
  appendU32(data, last);
  appendU32(data, glyphOffset);
}

void appendGlyph(std::vector<uint8_t>& data, const Glyph& glyph, uint32_t dataOffset) {
  data.push_back(glyph.width);
  data.push_back(glyph.height);
  appendU16(data, glyph.advanceX);
  appendI16(data, glyph.left);
  appendI16(data, glyph.top);
  appendU16(data, static_cast<uint16_t>(glyph.bitmap.size()));
  data.push_back(0);
  data.push_back(0);
  appendU32(data, dataOffset);
}

void appendKernEntry(std::vector<uint8_t>& data, const KernEntry& entry) {
  appendU16(data, entry.codepoint);
  data.push_back(entry.classId);
}

void appendLigature(std::vector<uint8_t>& data, const Ligature& ligature) {
  appendU32(data, ligature.pair);
  appendU32(data, ligature.replacement);
}

size_t styleTocOffset(size_t styleIndex) { return kTocStart + styleIndex * kTocEntrySize; }

SectionOffsets sectionOffsets(const std::vector<uint8_t>& data, size_t styleIndex) {
  const size_t toc = styleTocOffset(styleIndex);
  const size_t intervals = readU32(data, toc + kTocDataOffset);
  const size_t intervalBytes = static_cast<size_t>(readU32(data, toc + kTocIntervalCount)) * 12;
  const size_t glyphBytes = static_cast<size_t>(readU32(data, toc + kTocGlyphCount)) * 16;
  const size_t leftKernBytes = static_cast<size_t>(data[toc + kTocKernLeftEntryCount]) |
                               (static_cast<size_t>(data[toc + kTocKernLeftEntryCount + 1]) << 8);
  const size_t rightKernBytes = static_cast<size_t>(data[toc + kTocKernRightEntryCount]) |
                                (static_cast<size_t>(data[toc + kTocKernRightEntryCount + 1]) << 8);
  const size_t matrixBytes = static_cast<size_t>(data[toc + kTocKernLeftClassCount]) *
                             static_cast<size_t>(data[toc + kTocKernRightClassCount]);
  const size_t ligatureBytes = static_cast<size_t>(data[toc + kTocLigatureCount]) * 8;
  return {
      intervals,
      intervals + intervalBytes,
      intervals + intervalBytes + glyphBytes,
      intervals + intervalBytes + glyphBytes + leftKernBytes * 3,
      intervals + intervalBytes + glyphBytes + leftKernBytes * 3 + rightKernBytes * 3,
      intervals + intervalBytes + glyphBytes + leftKernBytes * 3 + rightKernBytes * 3 + matrixBytes,
      intervals + intervalBytes + glyphBytes + leftKernBytes * 3 + rightKernBytes * 3 + matrixBytes + ligatureBytes};
}

uint32_t glyphCountFromIntervals(const Style& style) {
  uint64_t count = 0;
  for (const auto& interval : style.intervals) {
    count += static_cast<uint64_t>(interval.second) - interval.first + 1;
  }
  expect(count <= std::numeric_limits<uint32_t>::max(), "test interval glyph count overflow");
  return static_cast<uint32_t>(count);
}

Style makeRichStyle(uint8_t id) {
  Style style{id,
              {{0x41, 0x42}, {0x61, 0x62}},
              {{3, 1, 48, 0, 8, {0xDC}}, {2, 2, 32, 0, 8, {0x1B}}, {4, 1, 64, 0, 8, {0xE4}}, {1, 1, 16, 0, 8, {0x40}}},
              16,
              12,
              -4,
              {{0x41, 1}, {0x61, 2}},
              {{0x42, 1}, {0x62, 2}},
              2,
              2,
              {-1, 0, 0, 1},
              {{0x00410042, 0xFB00}, {0x00610062, 0xFB01}}};
  return style;
}

Style makeSmallStyle(uint8_t id) {
  Style style;
  style.id = id;
  style.intervals = {{0x20, 0x20}};
  style.glyphs = {{2, 1, 32, 0, 8, {0xC0}}};
  style.advanceY = 15;
  style.ascender = 11;
  style.descender = -3;
  return style;
}

Style makeMaxKernClassStyle() {
  Style style;
  style.id = 0;
  style.intervals = {{0x0100, 0x01FF}};
  style.advanceY = 16;
  style.ascender = 12;
  style.descender = -4;
  style.leftClassCount = 255;
  style.rightClassCount = 1;
  style.kernMatrix.assign(255, 0);
  style.glyphs.reserve(256);
  for (uint32_t codepoint = 0x0100; codepoint <= 0x01FF; ++codepoint) {
    style.glyphs.push_back({1, 1, 16, 0, 1, {0x40}});
  }
  style.leftKern.reserve(255);
  for (uint16_t index = 0; index < 255; ++index) {
    style.leftKern.push_back({static_cast<uint16_t>(0x0100 + index), static_cast<uint8_t>(index + 1)});
  }
  style.rightKern = {{0x01FF, 1}};
  return style;
}

std::vector<uint8_t> buildCanonical(std::vector<Style> styles) {
  const size_t dataStart = kHeaderSize + styles.size() * kTocEntrySize;
  std::vector<uint8_t> data(dataStart, 0);
  const char magic[] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
  std::memcpy(data.data(), magic, sizeof(magic));
  writeU16(data, 8, 4);
  writeU16(data, 10, 1);
  data[12] = static_cast<uint8_t>(styles.size());

  size_t currentOffset = dataStart;
  for (size_t styleIndex = 0; styleIndex < styles.size(); ++styleIndex) {
    const Style& style = styles[styleIndex];
    const uint32_t intervalCount = static_cast<uint32_t>(style.intervals.size());
    const uint32_t glyphCount = glyphCountFromIntervals(style);
    expect(glyphCount == style.glyphs.size(), "canonical fixture glyph count disagrees with intervals");
    expect(style.leftKern.size() <= std::numeric_limits<uint16_t>::max() &&
               style.rightKern.size() <= std::numeric_limits<uint16_t>::max(),
           "canonical fixture kern entry count overflow");
    expect(style.kernMatrix.size() == static_cast<size_t>(style.leftClassCount) * style.rightClassCount,
           "canonical fixture matrix size disagrees with classes");

    const size_t toc = styleTocOffset(styleIndex);
    data[toc + kTocStyleId] = style.id;
    writeU32(data, toc + kTocIntervalCount, intervalCount);
    writeU32(data, toc + kTocGlyphCount, glyphCount);
    data[toc + 12] = style.advanceY;
    writeU16(data, toc + 13, static_cast<uint16_t>(style.ascender));
    writeU16(data, toc + 15, static_cast<uint16_t>(style.descender));
    writeU16(data, toc + kTocKernLeftEntryCount, static_cast<uint16_t>(style.leftKern.size()));
    writeU16(data, toc + kTocKernRightEntryCount, static_cast<uint16_t>(style.rightKern.size()));
    data[toc + kTocKernLeftClassCount] = style.leftClassCount;
    data[toc + kTocKernRightClassCount] = style.rightClassCount;
    data[toc + kTocLigatureCount] = static_cast<uint8_t>(style.ligatures.size());
    writeU32(data, toc + kTocDataOffset, static_cast<uint32_t>(currentOffset));

    for (const auto& interval : style.intervals) appendInterval(data, interval.first, interval.second, 0);
    uint32_t glyphOffset = 0;
    for (const Glyph& glyph : style.glyphs) {
      appendGlyph(data, glyph, glyphOffset);
      glyphOffset += static_cast<uint32_t>(glyph.bitmap.size());
    }
    for (const KernEntry& entry : style.leftKern) appendKernEntry(data, entry);
    for (const KernEntry& entry : style.rightKern) appendKernEntry(data, entry);
    data.insert(data.end(), style.kernMatrix.begin(), style.kernMatrix.end());
    for (const Ligature& ligature : style.ligatures) appendLigature(data, ligature);
    for (const Glyph& glyph : style.glyphs) data.insert(data.end(), glyph.bitmap.begin(), glyph.bitmap.end());

    const size_t expectedEnd = currentOffset + intervalCount * 12 + glyphCount * 16 +
                               (style.leftKern.size() + style.rightKern.size()) * 3 + style.kernMatrix.size() +
                               style.ligatures.size() * 8 + glyphOffset;
    expect(expectedEnd == data.size(), "canonical fixture section size mismatch");
    currentOffset = expectedEnd;

    const SectionOffsets offsets = sectionOffsets(data, styleIndex);
    for (size_t intervalIndex = 0; intervalIndex < style.intervals.size(); ++intervalIndex) {
      writeU32(data, offsets.intervals + intervalIndex * 12 + 8,
               static_cast<uint32_t>(std::accumulate(
                   style.intervals.begin(), style.intervals.begin() + intervalIndex, uint32_t{0},
                   [](uint32_t total, const auto& interval) { return total + interval.second - interval.first + 1; })));
    }
  }
  return data;
}

std::vector<uint8_t> makeSingleStyle() { return buildCanonical({makeRichStyle(0)}); }

std::vector<uint8_t> makeMultiStyle() { return buildCanonical({makeRichStyle(0), makeSmallStyle(2)}); }

struct ReadContext {
  const std::vector<uint8_t>* data;
};

int readAt(void* context, size_t offset, void* destination, size_t length) {
  const auto* readContext = static_cast<const ReadContext*>(context);
  const auto* data = readContext->data;
  if (offset > data->size() || length > data->size() - offset) return -1;
  if (length != 0) std::memcpy(destination, data->data() + offset, length);
  return static_cast<int>(length);
}

bool validate(const std::vector<uint8_t>& data) {
  ReadContext context{&data};
  const CpfontValidator::Reader reader{&context, data.size(), &readAt};
  return CpfontValidator::validateV4(reader);
}

void expectValid(const std::vector<uint8_t>& data, const std::string& name) {
  expect(validate(data), "Expected valid fixture: " + name);
}

void expectInvalid(std::vector<uint8_t> data, const std::string& name) {
  expect(!validate(data), "Expected invalid fixture: " + name);
}

void testCanonicalFiles() {
  expectValid(makeSingleStyle(), "single style");
  expectValid(makeMultiStyle(), "multiple styles");
  expectValid(buildCanonical({makeMaxKernClassStyle()}), "255 kern classes");
  expectValid(makeSingleStyle(), "exact EOF");
}

void testHeaderAndTocValidation() {
  auto data = makeSingleStyle();
  data[0] = 'X';
  expectInvalid(data, "bad magic");

  data = makeSingleStyle();
  writeU16(data, 8, 3);
  expectInvalid(data, "bad version");

  data = makeSingleStyle();
  writeU16(data, 10, 0);
  expectInvalid(data, "bad flags");

  data = makeSingleStyle();
  data[12] = 0;
  expectInvalid(data, "zero style count");

  data = makeSingleStyle();
  data[12] = 5;
  expectInvalid(data, "style count above maximum");

  data = makeSingleStyle();
  data[13] = 1;
  expectInvalid(data, "global reserved byte");

  data = makeSingleStyle();
  data[kTocStart + 1] = 1;
  expectInvalid(data, "TOC padding");

  data = makeSingleStyle();
  data[kTocStart + 28] = 1;
  expectInvalid(data, "TOC reserved bytes");

  data = makeSingleStyle();
  data[kTocStart + kTocStyleId] = 4;
  expectInvalid(data, "style id above maximum");

  data = makeMultiStyle();
  data[kTocStart + kTocStyleId] = 2;
  data[styleTocOffset(1) + kTocStyleId] = 0;
  expectInvalid(data, "unordered style ids");

  data = makeMultiStyle();
  data[styleTocOffset(1) + kTocStyleId] = 0;
  expectInvalid(data, "duplicate style ids");
}

void testTruncation() {
  auto data = makeMultiStyle();
  data.resize(kHeaderSize - 1);
  expectInvalid(data, "truncated header");

  data = makeMultiStyle();
  data.resize(kHeaderSize + kTocEntrySize - 1);
  expectInvalid(data, "truncated TOC");

  data = makeSingleStyle();
  const SectionOffsets offsets = sectionOffsets(data, 0);
  data.resize(offsets.intervals + 12 - 1);
  expectInvalid(data, "truncated interval");

  data = makeSingleStyle();
  data.resize(sectionOffsets(data, 0).glyphs + 16 - 1);
  expectInvalid(data, "truncated glyph");

  data = makeSingleStyle();
  data.resize(sectionOffsets(data, 0).bitmap + 4 - 1);
  expectInvalid(data, "truncated bitmap");
}

void testSectionLayoutAndOverflow() {
  auto data = makeMultiStyle();
  const size_t secondToc = styleTocOffset(1);
  writeU32(data, secondToc + kTocDataOffset, readU32(data, secondToc + kTocDataOffset) + 1);
  expectInvalid(data, "section offset gap");

  data = makeMultiStyle();
  writeU32(data, secondToc + kTocDataOffset, readU32(data, secondToc + kTocDataOffset) - 1);
  expectInvalid(data, "section offset overlap");

  data = makeSingleStyle();
  data.push_back(0);
  expectInvalid(data, "trailing bytes");

  data = makeSingleStyle();
  writeU32(data, kTocStart + kTocIntervalCount, std::numeric_limits<uint32_t>::max());
  expectInvalid(data, "count overflow");

  data = makeSingleStyle();
  writeU32(data, kTocStart + kTocDataOffset, std::numeric_limits<uint32_t>::max() - 3);
  expectInvalid(data, "section offset overflow");
}

void testIntervals() {
  auto data = makeSingleStyle();
  const SectionOffsets offsets = sectionOffsets(data, 0);
  writeU32(data, offsets.intervals + 4, 0x40);
  writeU32(data, offsets.intervals + 8, 0);
  expectInvalid(data, "interval first greater than last");

  data = makeSingleStyle();
  writeU32(data, offsets.intervals + 12, 0x3F);
  writeU32(data, offsets.intervals + 16, 0x40);
  expectInvalid(data, "interval order");

  data = makeSingleStyle();
  writeU32(data, offsets.intervals + 12, 0x42);
  expectInvalid(data, "interval overlap");

  data = makeSingleStyle();
  writeU32(data, offsets.intervals + 20, 3);
  expectInvalid(data, "interval offset mismatch");

  data = makeSingleStyle();
  writeU32(data, offsets.intervals + 8, 1);
  expectInvalid(data, "interval glyph total mismatch");

  data = makeSingleStyle();
  writeU32(data, offsets.intervals + 4, 0x50);
  expectInvalid(data, "interval span exceeds glyph total");

  data = makeSingleStyle();
  writeU32(data, offsets.intervals, 0);
  writeU32(data, offsets.intervals + 4, std::numeric_limits<uint32_t>::max());
  expectInvalid(data, "interval span overflow");

  data = makeSingleStyle();
  writeU32(data, offsets.intervals, 0x110000);
  expectInvalid(data, "interval codepoint outside Unicode");
}

void testGlyphs() {
  auto data = makeSingleStyle();
  const SectionOffsets offsets = sectionOffsets(data, 0);
  writeU16(data, offsets.glyphs + 8, 2);
  expectInvalid(data, "2-bit glyph data length");

  data = makeSingleStyle();
  data[offsets.glyphs + 10] = 1;
  expectInvalid(data, "glyph reserved padding");

  data = makeSingleStyle();
  writeU32(data, offsets.glyphs + 12, 1000);
  expectInvalid(data, "glyph bitmap offset bounds");

  data = makeSingleStyle();
  data[offsets.glyphs] = 5;
  writeU16(data, offsets.glyphs + 8, 2);
  writeU32(data, offsets.glyphs + 12, 3);
  expectInvalid(data, "glyph bitmap end bounds");
}

void testKernClasses() {
  auto data = makeSingleStyle();
  const size_t toc = kTocStart;
  writeU16(data, toc + kTocKernLeftEntryCount, 255);
  data[toc + kTocKernLeftClassCount] = 255;
  expectInvalid(data, "kern count section truncation");

  data = makeSingleStyle();
  data[toc + kTocKernLeftClassCount] = 255;
  data[toc + kTocKernRightClassCount] = 255;
  expectInvalid(data, "kern class matrix truncation");

  data = makeSingleStyle();
  const SectionOffsets offsets = sectionOffsets(data, 0);
  data[offsets.leftKern + 2] = 0;
  expectInvalid(data, "kern class id zero");

  data = makeSingleStyle();
  data[offsets.leftKern + 2] = 3;
  expectInvalid(data, "kern class id above count");

  data = makeSingleStyle();
  writeU16(data, offsets.leftKern + 3, 0x40);
  expectInvalid(data, "kern entries unsorted");

  data = makeSingleStyle();
  writeU16(data, offsets.leftKern + 3, 0x41);
  expectInvalid(data, "duplicate kern codepoint");

  data = makeSingleStyle();
  data[offsets.leftKern + 2] = 1;
  data[offsets.leftKern + 5] = 1;
  expectInvalid(data, "left kern class coverage");

  data = makeSingleStyle();
  data[offsets.rightKern + 2] = 1;
  data[offsets.rightKern + 5] = 1;
  expectInvalid(data, "right kern class coverage");
}

void testLigatures() {
  auto data = makeSingleStyle();
  const SectionOffsets offsets = sectionOffsets(data, 0);
  writeU32(data, offsets.ligatures + 8, 0x00400042);
  expectInvalid(data, "ligature sort order");

  data = makeSingleStyle();
  writeU32(data, offsets.ligatures + 8, readU32(data, offsets.ligatures));
  expectInvalid(data, "duplicate ligature pair");

  data = makeSingleStyle();
  writeU32(data, offsets.ligatures + 4, 0x110000);
  expectInvalid(data, "ligature replacement above Unicode");
}

}  // namespace

int main() {
  testCanonicalFiles();
  testHeaderAndTocValidation();
  testTruncation();
  testSectionLayoutAndOverflow();
  testIntervals();
  testGlyphs();
  testKernClasses();
  testLigatures();
  std::cout << "CPFONT_VALIDATOR_TEST_OK" << std::endl;
  return 0;
}
