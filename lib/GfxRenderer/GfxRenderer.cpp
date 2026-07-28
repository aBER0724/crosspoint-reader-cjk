#include "GfxRenderer.h"

#include <BidiUtils.h>
#include <BuildScratch.h>
#include <FontDecompressor.h>
#include <FontManager.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>

#include "ExternalFontHelpers.h"
#include "FontCacheManager.h"

// Built-in CJK UI font (embedded in flash) - 20px only
#include "cjk_ui_font_20.h"

// Reader font IDs (from fontIds.h) - used to determine when to use external
// Chinese font UI fonts should NOT use external font
namespace {
// UI font IDs that should NOT use external reader font
// Values must match src/fontIds.h
constexpr int UI_FONT_IDS[] = {
    22918846,     // UI_10_FONT_ID - for status display (battery, page number,
                  // etc.)
    1635686837,   // UI_12_FONT_ID - for status display
    -2089201234,  // UI_20_FONT_ID - primary UI font (menus, titles, settings)
    674098198     // SMALL_FONT_ID
};
constexpr int UI_FONT_COUNT = sizeof(UI_FONT_IDS) / sizeof(UI_FONT_IDS[0]);

// Reader font IDs - values must match src/fontIds.h
constexpr int READER_FONT_IDS[] = {
    85340443,     // NOTOSERIF_12_FONT_ID
    -1367885987,  // NOTOSERIF_14_FONT_ID
    1428909134,   // NOTOSERIF_16_FONT_ID
    -501438527,   // NOTOSERIF_18_FONT_ID
    2057568286,   // NOTOSANS_12_FONT_ID
    -1589315735,  // NOTOSANS_14_FONT_ID
    1669013660,   // NOTOSANS_16_FONT_ID
    37077304,     // NOTOSANS_18_FONT_ID
    -853313197,   // OPENDYSLEXIC_8_FONT_ID
    963754926,    // OPENDYSLEXIC_10_FONT_ID
    858950283,    // OPENDYSLEXIC_12_FONT_ID
    1877344218    // OPENDYSLEXIC_14_FONT_ID
};
constexpr int READER_FONT_COUNT = sizeof(READER_FONT_IDS) / sizeof(READER_FONT_IDS[0]);

uint8_t resolveSdCardStyle(const SdCardFont& font, const EpdFontFamily::Style style) {
  return font.resolveStyle(static_cast<uint8_t>(style));
}

bool isHalfScaleStyle(const EpdFontFamily::Style style) {
  return (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
}

int snapTextAdvance(const int32_t advanceFP, const bool halfScale) {
  if (!halfScale) return fp4::toPixel(advanceFP);

  constexpr int32_t HALF_SCALE_ROUNDING = 1 << fp4::FRAC_BITS;
  return static_cast<int>((advanceFP + HALF_SCALE_ROUNDING) >> (fp4::FRAC_BITS + 1));
}

int scalePositiveTextAdvance(const int advance, const bool halfScale) {
  return halfScale ? (advance + 1) / 2 : advance;
}

// CJK built-in font renders with 4px descent below baseline. External fonts
// using the non-rich metrics format set metrics.top = charHeight, which places
// the glyph bottom exactly at the baseline. To align with built-in CJK, the
// baseline for non-rich external font glyphs must be shifted down by this
// descent amount so both paths share the same visual baseline.
static constexpr int CJK_UI_FONT_DESCENT = 4;
constexpr unsigned long DARK_UI_REDRIVE_MIN_INTERVAL_MS = 30000;
constexpr uint8_t DARK_UI_FAST_REFRESHES_PER_REDRIVE = 32;

// Check if a Unicode codepoint is CJK (Chinese/Japanese/Korean)
// Only these characters should use the external font width
bool isCjkCodepoint(const uint32_t cp) {
  // CJK Unified Ideographs: U+4E00 - U+9FFF
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  // CJK Unified Ideographs Extension A: U+3400 - U+4DBF
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;
  // CJK Punctuation: U+3000 - U+303F
  if (cp >= 0x3000 && cp <= 0x303F) return true;
  // Hiragana: U+3040 - U+309F
  if (cp >= 0x3040 && cp <= 0x309F) return true;
  // Katakana: U+30A0 - U+30FF
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;
  // CJK Compatibility Ideographs: U+F900 - U+FAFF
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;
  // Fullwidth forms: U+FF00 - U+FFEF
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
  // General Punctuation: U+2000 - U+206F (includes smart quotes, ellipsis,
  // dashes)
  if (cp >= 0x2000 && cp <= 0x206F) return true;
  // Number Forms: U+2150 - U+218F (includes Roman numerals)
  if (cp >= 0x2150 && cp <= 0x218F) return true;
  // Enclosed Alphanumerics: U+2460 - U+24FF
  if (cp >= 0x2460 && cp <= 0x24FF) return true;
  // Enclosed CJK Letters and Months: U+3200 - U+32FF
  if (cp >= 0x3200 && cp <= 0x32FF) return true;
  // CJK Compatibility: U+3300 - U+33FF
  if (cp >= 0x3300 && cp <= 0x33FF) return true;
  return false;
}

bool isAsciiDigit(const uint32_t cp) { return cp >= '0' && cp <= '9'; }

bool isAsciiLetter(const uint32_t cp) { return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'); }

bool hasUiGlyphForText(const char* text) {
  if (text == nullptr || *text == '\0') {
    return false;
  }

  const char* ptr = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
    if (CjkUiFont20::hasCjkUiGlyph(cp)) {
      return true;
    }
  }
  return false;
}

// RAII pair of heap buffers used by the BMP rendering paths.
struct BitmapRowBuffers {
  uint8_t* outputRow = nullptr;
  uint8_t* rowBytes = nullptr;

  BitmapRowBuffers(const int outputRowSize, const int rowBytesSize)
      : outputRow(static_cast<uint8_t*>(malloc(outputRowSize))),
        rowBytes(static_cast<uint8_t*>(malloc(rowBytesSize))) {}
  ~BitmapRowBuffers() {
    free(outputRow);
    free(rowBytes);
  }
  BitmapRowBuffers(const BitmapRowBuffers&) = delete;
  BitmapRowBuffers& operator=(const BitmapRowBuffers&) = delete;

  bool ok() const { return outputRow != nullptr && rowBytes != nullptr; }
};

bool isUiFont(const int fontId) {
  for (int i = 0; i < UI_FONT_COUNT; i++) {
    if (UI_FONT_IDS[i] == fontId) {
      return true;
    }
  }
  return false;
}
}  // namespace

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, BidiUtils::BidiBaseDir baseDir);

// Appends the shaped visual form of every RTL token in `text` to `shapedOut`.
// getTextAdvanceX() measures the bidi-reordered, Arabic-shaped codepoint stream,
// so the SD advance table must be warmed with the presentation forms as well as
// the logical codepoints — otherwise every RTL word measurement misses the fast
// path and falls through to onGlyphMiss(), which opens the .cpfont and reads
// glyph metadata + bitmap into the 8-slot overflow ring, once per glyph.
// Tokens without RTL lead bytes (0xD6-0xDB) are skipped with a byte scan, so
// pure-LTR text pays almost nothing.
void appendShapedRtlTokens(const char* text, std::string& shapedOut) {
  const auto isBreak = [](const char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
  std::string token;
  std::string visual;
  const char* p = text;
  while (*p) {
    while (*p && isBreak(*p)) ++p;
    const char* start = p;
    bool hasRtlBytes = false;
    while (*p && !isBreak(*p)) {
      const auto b = static_cast<unsigned char>(*p);
      hasRtlBytes = hasRtlBytes || (b >= 0xD6 && b <= 0xDB);
      ++p;
    }
    if (!hasRtlBytes) continue;
    token.assign(start, p - start);
    if (BidiUtils::applyBidiVisual(token.c_str(), visual, static_cast<int>(BidiUtils::BidiBaseDir::AUTO))) {
      shapedOut += visual;
    }
  }
}
}  // namespace

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  if (fontData->groups != nullptr) {
    auto* fd = fontCacheManager_ ? fontCacheManager_->getDecompressor() : nullptr;
    if (!fd) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
    // For page-buffer hits the pointer is stable for the page lifetime.
    // For hot-group hits it is valid only until the next getBitmap() call — callers
    // must consume it (draw the glyph) before requesting another bitmap.
    return fd->getBitmap(fontData, glyph, glyphIndex);
  }
  if (fontData->glyphMissCtx) {
    auto* sdFont = SdCardFont::fromMissCtx(fontData->glyphMissCtx);
    if (sdFont->isOverflowGlyph(glyph)) {
      return sdFont->getOverflowBitmap(glyph);
    }
  }
  return &fontData->bitmap[glyph->dataOffset];
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    std::string shaped;
    appendShapedRtlTokens(utf8Text, shaped);
    int missed = it->second->buildAdvanceTable(utf8Text, styleMask, shaped.empty() ? nullptr : shaped.c_str());
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const std::vector<std::string>& words, bool includeHyphen,
                                        uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    // Augment the persistent advance-only table for layout measurement.
    // The table survives across paragraphs/sections (capped per font), so
    // repeated indexing of the same SD font amortizes glyph-metric SD reads.
    std::string shaped;
    for (const auto& w : words) {
      appendShapedRtlTokens(w.c_str(), shaped);
    }
    int missed =
        it->second->buildAdvanceTable(words, includeHyphen, styleMask, shaped.empty() ? nullptr : shaped.c_str());
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! No framebuffer");
    assert(false);
  }
  panelWidth = display.getDisplayWidth();
  panelHeight = display.getDisplayHeight();
  panelWidthBytes = display.getDisplayWidthBytes();
  frameBufferSize = display.getBufferSize();
  bwBufferChunks.assign((frameBufferSize + BW_BUFFER_CHUNK_SIZE - 1) / BW_BUFFER_CHUNK_SIZE, nullptr);
}

void GfxRenderer::releaseFrameBufferForBuild() {
  // Lend the framebuffer's bytes IN PLACE: the allocation is never freed, so
  // it cannot move and repeated loans cannot fragment the heap (the previous
  // free+realloc model measurably decayed the max contiguous block over a
  // session). The bytes are deposited in the build-scratch registry so
  // memory-hungry build phases (e.g. InflateStream's tinfl state + window)
  // can claim them instead of allocating.
  uint32_t size = 0;
  uint8_t* scratch = display.lendFrameBufferStorage(&size);
  frameBuffer = nullptr;
  if (scratch) {
    buildscratch::lend(scratch, size);
  }
}

bool GfxRenderer::restoreFrameBufferAfterBuild() {
  buildscratch::reclaim();
  display.returnFrameBufferStorage();  // cannot fail: the allocation was never freed
  frameBuffer = display.getFrameBuffer();
  return frameBuffer != nullptr;
}

GfxRenderer::FrameBufferLoan::FrameBufferLoan(GfxRenderer& renderer) : renderer_(renderer) {
  // Nesting guard: if the framebuffer is already lent out (an outer loan),
  // stay inert so this end() cannot return storage the outer loan still owns.
  if (!renderer_.hasFrameBuffer()) return;
  renderer_.releaseFrameBufferForBuild();
  active_ = true;
}

void GfxRenderer::FrameBufferLoan::end() {
  if (!active_) return;
  active_ = false;
  if (!renderer_.restoreFrameBufferAfterBuild()) {
    // Only reachable if the framebuffer never existed, which begin() already
    // asserts against; kept as a backstop since running blind helps nobody.
    LOG_ERR("GFX", "Framebuffer restore failed - restarting");
    ESP.restart();
  }
}

bool GfxRenderer::isFontCacheScanning() const { return fontCacheManager_ && fontCacheManager_->isScanning(); }

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  auto result = fontMap.insert({fontId, font});
  if (!result.second) {
    LOG_ERR("GFX", "Font ID %d already registered, ignoring duplicate", fontId);
  }
}

int GfxRenderer::resolveTextFontId(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (fallbackFontMap_.empty() || text == nullptr || *text == '\0') {
    return fontId;
  }
  const auto fbIt = fallbackFontMap_.find(fontId);
  if (fbIt == fallbackFontMap_.end()) {
    return fontId;  // no fallback registered for this font
  }
  const int fallbackFontId = fbIt->second;
  const auto fontIt = fontMap.find(fontId);
  const auto fallbackIt = fontMap.find(fallbackFontId);
  if (fontIt == fontMap.end() || fallbackIt == fontMap.end()) {
    return fontId;  // unknown primary or fallback not loaded — let the caller handle it
  }
  const EpdFontFamily& primary = fontIt->second;
  const EpdFontFamily& fallback = fallbackIt->second;
  const char* cursor = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor)))) {
    // Only redirect for CJK the primary font cannot draw but the fallback can.
    // Latin/symbol strings the built-in UI fonts already cover are left
    // untouched, and a partial-coverage fallback (e.g. kana-only) is not worth
    // dragging the whole string into for glyphs it would also miss.
    if (utf8IsCjkCodepoint(cp) && !primary.hasCodepoint(cp, style) && fallback.hasCodepoint(cp, style)) {
      return fallbackFontId;
    }
  }
  return fontId;
}

// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY, const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

// Output of screenRectToAlignedMemRect: a rectangle in panel-memory
// coordinates whose x and width are guaranteed to be multiples of 8 (the
// SDK's EInkDisplay::displayWindow alignment requirement). `valid == false`
// means the input was empty or fully outside the panel.
struct AlignedMemRect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  bool valid = false;
};

// Translate a screen-coordinate rectangle (the coordinate system used by
// fillRect / drawText / the rest of the renderer's public API) into a
// panel-memory rectangle suitable for direct framebuffer indexing. Rotates
// the rectangle's two opposite corners with rotateCoordinates(), takes the
// bounding box (which naturally swaps width/height in Portrait /
// PortraitInverted), then snaps the x extent outward to multiples of 8 and
// clamps to panel bounds. Precondition: panel dims are multiples of 8 (true
// for the 800x480 panel), so clamping cannot re-break alignment.
static AlignedMemRect screenRectToAlignedMemRect(GfxRenderer::Orientation orientation, int sx, int sy, int sw, int sh,
                                                 uint16_t panelWidth, uint16_t panelHeight) {
  AlignedMemRect out;
  if (sw <= 0 || sh <= 0) return out;

  int x0, y0, x1, y1;
  rotateCoordinates(orientation, sx, sy, &x0, &y0, panelWidth, panelHeight);
  rotateCoordinates(orientation, sx + sw - 1, sy + sh - 1, &x1, &y1, panelWidth, panelHeight);

  const int memXLo = std::min(x0, x1);
  const int memYLo = std::min(y0, y1);
  const int memXHi = std::max(x0, x1) + 1;  // exclusive upper bound
  const int memYHi = std::max(y0, y1) + 1;

  // Snap x outward to multiples of 8.
  int alignedXLo = memXLo & ~0x7;        // round down
  int alignedXHi = (memXHi + 7) & ~0x7;  // round up

  if (alignedXLo < 0) alignedXLo = 0;
  if (alignedXHi > panelWidth) alignedXHi = panelWidth;
  int clampedYLo = memYLo;
  int clampedYHi = memYHi;
  if (clampedYLo < 0) clampedYLo = 0;
  if (clampedYHi > panelHeight) clampedYHi = panelHeight;

  if (alignedXHi <= alignedXLo || clampedYHi <= clampedYLo) return out;

  out.x = static_cast<uint16_t>(alignedXLo);
  out.y = static_cast<uint16_t>(clampedYLo);
  out.w = static_cast<uint16_t>(alignedXHi - alignedXLo);
  out.h = static_cast<uint16_t>(clampedYHi - clampedYLo);
  out.valid = true;
  return out;
}

struct PhysicalRect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

bool logicalRectToPhysicalWindow(const GfxRenderer::Orientation orientation, int x, int y, int width, int height,
                                 const int screenWidth, const int screenHeight, const uint16_t panelWidth,
                                 const uint16_t panelHeight, PhysicalRect* out) {
  if (out == nullptr || width <= 0 || height <= 0) {
    return false;
  }

  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(screenWidth - 1, x + width - 1);
  const int y1 = std::min(screenHeight - 1, y + height - 1);
  if (x0 > x1 || y0 > y1) {
    return false;
  }

  int px0 = 0;
  int py0 = 0;
  int px1 = 0;
  int py1 = 0;
  int px2 = 0;
  int py2 = 0;
  int px3 = 0;
  int py3 = 0;
  rotateCoordinates(orientation, x0, y0, &px0, &py0, panelWidth, panelHeight);
  rotateCoordinates(orientation, x1, y0, &px1, &py1, panelWidth, panelHeight);
  rotateCoordinates(orientation, x0, y1, &px2, &py2, panelWidth, panelHeight);
  rotateCoordinates(orientation, x1, y1, &px3, &py3, panelWidth, panelHeight);

  int minX = std::min({px0, px1, px2, px3});
  int maxX = std::max({px0, px1, px2, px3});
  int minY = std::min({py0, py1, py2, py3});
  int maxY = std::max({py0, py1, py2, py3});

  minX = std::max(0, minX);
  maxX = std::min(static_cast<int>(panelWidth) - 1, maxX);
  minY = std::max(0, minY);
  maxY = std::min(static_cast<int>(panelHeight) - 1, maxY);
  if (minX > maxX || minY > maxY) {
    return false;
  }

  const int alignedX = (minX / 8) * 8;
  const int alignedEndX = std::min(static_cast<int>(panelWidth), ((maxX + 8) / 8) * 8);
  if (alignedX >= alignedEndX) {
    return false;
  }

  out->x = static_cast<uint16_t>(alignedX);
  out->y = static_cast<uint16_t>(minY);
  out->width = static_cast<uint16_t>(alignedEndX - alignedX);
  out->height = static_cast<uint16_t>(maxY - minY + 1);
  return out->width > 0 && out->height > 0;
}

enum class TextRotation { None, Rotated90CW };

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
// Render a glyph at 50% scale. Used for SUP/SUB style bits.
//
// Each destination pixel represents a 2x2 source block. Drawing when that block
// contains ink preserves thin strokes that nearest-neighbor sampling can skip.
//
// The advance width is also halved in drawText() so layout reserves exactly the right
// horizontal space for the scaled glyph.
template <TextRotation rotation>
static void renderCharScaled(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                             const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                             const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) return;

  const EpdFontData* fontData = fontFamily.getData(style);
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (!bitmap) return;

  const int srcW = glyph->width;
  const int srcH = glyph->height;
  const int dstW = (srcW + 1) / 2;  // ceil so odd-width glyphs aren't clipped
  const int dstH = (srcH + 1) / 2;
  const int outerBase = rotation == TextRotation::Rotated90CW ? cursorX + fontData->ascender / 2 - glyph->top / 2
                                                              : cursorY - glyph->top / 2;
  const int innerBase = rotation == TextRotation::Rotated90CW ? cursorY - glyph->left / 2 : cursorX + glyph->left / 2;

  if constexpr (rotation == TextRotation::Rotated90CW) {
    if (!renderer.glyphIntersectsStrip(outerBase, innerBase - (dstW - 1), outerBase + dstH - 1, innerBase)) {
      return;
    }
  } else if (!renderer.glyphIntersectsStrip(innerBase, outerBase, innerBase + dstW - 1, outerBase + dstH - 1)) {
    return;
  }

  for (int dstY = 0; dstY < dstH; dstY++) {
    const int srcY = dstY * 2;
    for (int dstX = 0; dstX < dstW; dstX++) {
      const int srcX = dstX * 2;
      bool shouldDraw = false;
      bool outputState = pixelState;
      if (fontData->is2Bit) {
        bool hasBwInk = false;
        bool hasMsbInk = false;
        bool hasLsbInk = false;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            const uint8_t byte = bitmap[pos >> 2];
            const uint8_t raw = (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
            hasBwInk = hasBwInk || raw > 0;
            hasMsbInk = hasMsbInk || raw == 1 || raw == 2;
            hasLsbInk = hasLsbInk || raw == 2;
          }
        }
        shouldDraw = (renderMode == GfxRenderer::BW && hasBwInk) ||
                     (renderMode == GfxRenderer::GRAYSCALE_MSB && hasMsbInk) ||
                     (renderMode == GfxRenderer::GRAYSCALE_LSB && hasLsbInk);
        outputState = renderMode == GfxRenderer::BW ? pixelState : false;
      } else {
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            if ((bitmap[pos >> 3] >> (7 - (pos & 7))) & 1) shouldDraw = true;
          }
        }
      }

      if (shouldDraw) {
        if constexpr (rotation == TextRotation::Rotated90CW) {
          renderer.drawPixel(outerBase + dstY, innerBase - dstX, outputState);
        } else {
          renderer.drawPixel(innerBase + dstX, outerBase + dstY, outputState);
        }
      }
    }
  }
}
template <TextRotation rotation>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

  // Tiled-grayscale band culling: if this glyph's physical y-extent is entirely
  // outside the active strip, skip it before the expensive bitmap decode. This
  // is what makes per-band re-rendering cheap. No-op outside strip mode.
  if constexpr (rotation == TextRotation::Rotated90CW) {
    const int ob = cursorX + fontData->ascender - top;
    const int ib = cursorY - left;
    if (!renderer.glyphIntersectsStrip(ob, ib - (width - 1), ob + height - 1, ib)) {
      return;
    }
  } else {
    const int gx0 = cursorX + left;
    const int gy0 = cursorY - top;
    if (!renderer.glyphIntersectsStrip(gx0, gy0, gx0 + width - 1, gy0 + height - 1)) {
      return;
    }
  }
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);

  if (bitmap != nullptr) {
    // For Normal:  outer loop advances screenY, inner loop advances screenX
    // For Rotated: outer loop advances screenX, inner loop advances screenY (in reverse)
    int outerBase, innerBase;
    if constexpr (rotation == TextRotation::Rotated90CW) {
      outerBase = cursorX + fontData->ascender - top;  // screenX = outerBase + glyphY
      innerBase = cursorY - left;                      // screenY = innerBase - glyphX
    } else {
      outerBase = cursorY - top;   // screenY = outerBase + glyphY
      innerBase = cursorX + left;  // screenX = innerBase + glyphX
    }

    if (is2Bit) {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 2];
          const uint8_t bit_index = (3 - (pixelPosition & 3)) * 2;
          // the direct bit from the font is 0 -> white, 1 -> light gray, 2 -> dark gray, 3 -> black
          // we swap this to better match the way images and screen think about colors:
          // 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white
          const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

          if (renderMode == GfxRenderer::BW && bmpVal < 3) {
            // Black (also paints over the grays in BW mode)
            renderer.drawPixel(screenX, screenY, pixelState);
          } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
            // Light gray (also mark the MSB if it's going to be a dark gray too)
            // We have to flag pixels in reverse for the gray buffers, as 0 leave alone, 1 update
            renderer.drawPixel(screenX, screenY, false);
          } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && bmpVal == 1) {
            // Dark gray
            renderer.drawPixel(screenX, screenY, false);
          }
        }
      }
    } else {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 3];
          const uint8_t bit_index = 7 - (pixelPosition & 7);

          if ((byte >> bit_index) & 1) {
            renderer.drawPixel(screenX, screenY, pixelState);
          }
        }
      }
    }
  }
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);

  // Bounds checking against runtime panel dimensions
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) {
    // Limit log output to avoid performance issues (log only first few per
    // session)
    static int outsideRangeCount = 0;
    if (outsideRangeCount < 5) {
      LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
      outsideRangeCount++;
      if (outsideRangeCount == 5) {
        LOG_ERR("GFX", "!! Suppressing further outside range warnings");
      }
    }
    return;
  }

  // Calculate byte position and bit position
  const uint32_t byteIndex = static_cast<uint32_t>(phyY) * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  // In dark mode, invert the pixel state (black <-> white)
  // But NOT in grayscale mode - grayscale rendering uses special pixel marking
  // And NOT when skipDarkModeForImages is set - cover art should keep original
  // colors
  const bool shouldInvert = darkMode && !skipDarkModeForImages && renderMode == BW;
  const bool actualState = shouldInvert ? !state : state;

  if (actualState) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);  // Clear bit = black pixel
  } else {
    frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit = white pixel
  }
}

bool GfxRenderer::resolveTextFallback(const int fontId, const uint32_t cp, const bool loadBitmap,
                                      TextFallbackGlyph* out) const {
  if (!out) return false;
  *out = TextFallbackGlyph{};

  FontManager& fm = FontManager::getInstance();
  const bool isCjk = isCjkCodepoint(cp);

  enum class MetricsMode { Reader, Ui, ReaderAsUi };
  const auto tryExternal = [&](ExternalFont* extFont, const MetricsMode mode, const int baselineOffset,
                               const int cellClipWidth) {
    if (!extFont || !extFont->isLoaded()) return false;

    ExternalGlyphMetrics metrics{};
    metrics.width = extFont->getCharWidth();
    metrics.height = extFont->getCharHeight();
    metrics.advanceX = extFont->getCharWidth();
    const uint8_t* bitmap = nullptr;
    if (loadBitmap) {
      bitmap = extFont->getGlyph(cp);
      if (!bitmap) return false;
      metrics = getDefaultMetrics(*extFont, cp);
    } else if (!extFont->getGlyphMetricsForLayout(cp, &metrics)) {
      return false;
    }

    int advance = metrics.advanceX;
    if (mode == MetricsMode::Reader) {
      if (shouldUseCjkSymbolCellMetrics(cp)) {
        normalizeCjkSymbolMetricsForRendering(metrics, extFont->getCharWidth(), extFont->isRichMetricsFormat());
      }
      const int spacing = isCjk ? cjkSpacing : getAsciiSpacing(cp);
      advance = getExternalGlyphAdvanceForRendering(metrics, extFont->getCharWidth(), spacing, isCjk,
                                                    shouldUseGlyphBoundsForAdvance(cp));
    } else if (mode == MetricsMode::Ui) {
      if (shouldUseCjkSymbolCellMetrics(cp)) {
        normalizeCjkSymbolMetricsForRendering(metrics, extFont->getCharWidth(), extFont->isRichMetricsFormat());
        advance = getExternalGlyphAdvanceForRendering(metrics, extFont->getCharWidth(), 0, true, false);
      } else {
        advance = adjustNonRichAdvance(metrics, *extFont);
      }
    } else if (shouldUseCjkSymbolCellMetrics(cp)) {
      normalizeCjkSymbolMetricsForRendering(metrics, extFont->getCharWidth(), extFont->isRichMetricsFormat());
    }

    out->kind = TextFallbackGlyph::Kind::External;
    out->font = extFont;
    out->bitmap = bitmap;
    out->metrics = metrics;
    out->advance = advance;
    out->baselineOffset = baselineOffset;
    out->cellClipWidth = cellClipWidth;
    return true;
  };

  const auto useBuiltinCjk = [&]() {
    if (!CjkUiFont20::hasCjkUiGlyph(cp)) return false;
    out->kind = TextFallbackGlyph::Kind::BuiltinCjk;
    out->advance = CjkUiFont20::getCjkUiGlyphWidth(cp);
    out->metrics.width = CjkUiFont20::CJK_UI_FONT_WIDTH;
    out->metrics.height = CjkUiFont20::CJK_UI_FONT_HEIGHT;
    out->metrics.advanceX = out->advance;
    out->metrics.top = CjkUiFont20::CJK_UI_FONT_HEIGHT - CJK_UI_FONT_DESCENT;
    return out->advance > 0;
  };

  if (isReaderFont(fontId)) {
    ExternalFont* readerFont = fm.isExternalFontEnabled() ? fm.getActiveFont() : nullptr;
    const int cellClipWidth = isCjk && readerFont ? readerFont->getCharWidth() : -1;
    if (tryExternal(readerFont, MetricsMode::Reader, 0, cellClipWidth)) return true;
    return isCjk && useBuiltinCjk();
  }

  if (isCjk) {
    if (fm.isUiFontEnabled() && tryExternal(fm.getActiveUiFont(), MetricsMode::Ui, 0, -1)) return true;
    if (useBuiltinCjk()) return true;
    if (fm.isExternalFontEnabled()) {
      ExternalFont* readerFont = fm.getActiveFont();
      const int baselineOffset = readerFont && !readerFont->isRichMetricsFormat() ? CJK_UI_FONT_DESCENT : 0;
      if (tryExternal(readerFont, MetricsMode::ReaderAsUi, baselineOffset, -1)) return true;
    }
    return false;
  }

  if (fm.isUiFontEnabled() && tryExternal(fm.getActiveUiFont(), MetricsMode::Ui, 0, -1)) return true;
  return useBuiltinCjk();
}

void GfxRenderer::renderTextFallback(const TextFallbackGlyph& fallback, const uint32_t cp, int* x, const int baselineY,
                                     const bool pixelState, const bool halfScale) const {
  if (!halfScale) {
    if (fallback.kind == TextFallbackGlyph::Kind::External) {
      renderExternalGlyph(fallback.bitmap, fallback.font, x, baselineY + fallback.baselineOffset, pixelState,
                          fallback.metrics, fallback.advance, fallback.cellClipWidth);
    } else if (fallback.kind == TextFallbackGlyph::Kind::BuiltinCjk) {
      renderBuiltinCjkGlyph(cp, x, baselineY, pixelState);
    }
    return;
  }

  if (fallback.kind == TextFallbackGlyph::Kind::External) {
    if (!fallback.bitmap || !fallback.font) return;

    const int srcWidth = fallback.metrics.width > 0 ? fallback.metrics.width : fallback.font->getCharWidth();
    const int srcHeight = fallback.metrics.height > 0 ? fallback.metrics.height : fallback.font->getCharHeight();
    const int bytesPerRow = (srcWidth + 7) / 8;
    const int drawX = *x + fallback.metrics.left / 2;
    const int drawY = baselineY + fallback.baselineOffset / 2 - fallback.metrics.top / 2;
    const int dstWidth = (srcWidth + 1) / 2;
    const int dstHeight = (srcHeight + 1) / 2;
    const int scaledCellClipWidth =
        fallback.cellClipWidth > 0 ? (fallback.cellClipWidth + 1) / 2 : fallback.cellClipWidth;

    for (int dstY = 0; dstY < dstHeight; dstY++) {
      const int screenY = drawY + dstY;
      if (screenY < 0 || screenY >= getScreenHeight()) continue;

      for (int dstX = 0; dstX < dstWidth; dstX++) {
        const int screenX = drawX + dstX;
        if (screenX < 0 || screenX >= getScreenWidth()) continue;
        if (scaledCellClipWidth > 0 && (screenX < *x || screenX >= *x + scaledCellClipWidth)) continue;

        const int srcX = dstX * 2;
        const int srcY = dstY * 2;
        bool hasInk = false;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcHeight; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcWidth; sampleX++) {
            const int pixelX = srcX + sampleX;
            const int byteIndex = (srcY + sampleY) * bytesPerRow + pixelX / 8;
            const int bitIndex = 7 - pixelX % 8;
            if ((fallback.bitmap[byteIndex] >> bitIndex) & 1) hasInk = true;
          }
        }
        if (hasInk) drawPixel(screenX, screenY, pixelState);
      }
    }
    *x += std::max(1, (fallback.advance + 1) / 2);
  } else if (fallback.kind == TextFallbackGlyph::Kind::BuiltinCjk) {
    const uint8_t* bitmap = CjkUiFont20::getCjkUiGlyph(cp);
    const int srcWidth = CjkUiFont20::CJK_UI_FONT_WIDTH;
    const int srcHeight = CjkUiFont20::CJK_UI_FONT_HEIGHT;
    const int bytesPerRow = CjkUiFont20::CJK_UI_FONT_BYTES_PER_ROW;
    if (!bitmap || fallback.advance <= 0) return;

    const int drawY = baselineY - (srcHeight - CJK_UI_FONT_DESCENT) / 2;
    const int dstWidth = (srcWidth + 1) / 2;
    const int dstHeight = (srcHeight + 1) / 2;
    for (int dstY = 0; dstY < dstHeight; dstY++) {
      const int screenY = drawY + dstY;
      if (screenY < 0 || screenY >= getScreenHeight()) continue;

      for (int dstX = 0; dstX < dstWidth; dstX++) {
        const int screenX = *x + dstX;
        if (screenX < 0 || screenX >= getScreenWidth()) continue;

        const int srcX = dstX * 2;
        const int srcY = dstY * 2;
        bool hasInk = false;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcHeight; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcWidth; sampleX++) {
            const int pixelX = srcX + sampleX;
            const int byteIndex = (srcY + sampleY) * bytesPerRow + pixelX / 8;
            const int bitIndex = 7 - pixelX % 8;
            if ((pgm_read_byte(&bitmap[byteIndex]) >> bitIndex) & 1) hasInk = true;
          }
        }
        if (hasInk) drawPixel(screenX, screenY, pixelState);
      }
    }
    *x += std::max(1, (fallback.advance + 1) / 2);
  }
}

void GfxRenderer::renderTextFallbackRotated90CW(const TextFallbackGlyph& fallback, const uint32_t cp, const int cursorX,
                                                const int cursorY, const bool pixelState, const bool halfScale) const {
  const int scale = halfScale ? 2 : 1;
  const auto renderBitmap = [&](const uint8_t* bitmap, const int srcWidth, const int srcHeight, const int bytesPerRow,
                                const int ascender, const int top, const int left, const int cellClipWidth,
                                const bool progmemBitmap) {
    if (!bitmap || srcWidth <= 0 || srcHeight <= 0) return;

    const int dstWidth = (srcWidth + scale - 1) / scale;
    const int dstHeight = (srcHeight + scale - 1) / scale;
    const int outerBase = cursorX + ascender / scale - top / scale;
    const int innerBase = cursorY - left / scale;
    const int scaledCellClipWidth = cellClipWidth > 0 ? (cellClipWidth + scale - 1) / scale : cellClipWidth;
    if (!glyphIntersectsStrip(outerBase, innerBase - (dstWidth - 1), outerBase + dstHeight - 1, innerBase)) return;

    for (int dstY = 0; dstY < dstHeight; dstY++) {
      const int screenX = outerBase + dstY;
      if (screenX < 0 || screenX >= getScreenWidth()) continue;

      for (int dstX = 0; dstX < dstWidth; dstX++) {
        const int screenY = innerBase - dstX;
        if (screenY < 0 || screenY >= getScreenHeight()) continue;
        if (scaledCellClipWidth > 0 && (screenY > cursorY || screenY <= cursorY - scaledCellClipWidth)) continue;

        const int srcX = dstX * scale;
        const int srcY = dstY * scale;
        bool hasInk = false;
        for (int sampleY = 0; sampleY < scale && srcY + sampleY < srcHeight; sampleY++) {
          for (int sampleX = 0; sampleX < scale && srcX + sampleX < srcWidth; sampleX++) {
            const int pixelX = srcX + sampleX;
            const int byteIndex = (srcY + sampleY) * bytesPerRow + pixelX / 8;
            const int bitIndex = 7 - pixelX % 8;
            const uint8_t byte = progmemBitmap ? pgm_read_byte(&bitmap[byteIndex]) : bitmap[byteIndex];
            if ((byte >> bitIndex) & 1) hasInk = true;
          }
        }
        if (hasInk) drawPixel(screenX, screenY, pixelState);
      }
    }
  };

  if (fallback.kind == TextFallbackGlyph::Kind::External) {
    if (!fallback.font) return;
    const int srcWidth = fallback.metrics.width > 0 ? fallback.metrics.width : fallback.font->getCharWidth();
    const int srcHeight = fallback.metrics.height > 0 ? fallback.metrics.height : fallback.font->getCharHeight();
    renderBitmap(fallback.bitmap, srcWidth, srcHeight, (srcWidth + 7) / 8,
                 getExternalFontAscenderForRendering(*fallback.font) + fallback.baselineOffset, fallback.metrics.top,
                 fallback.metrics.left, fallback.cellClipWidth, false);
  } else if (fallback.kind == TextFallbackGlyph::Kind::BuiltinCjk) {
    const uint8_t* bitmap = CjkUiFont20::getCjkUiGlyph(cp);
    const int srcWidth = CjkUiFont20::CJK_UI_FONT_WIDTH;
    const int srcHeight = CjkUiFont20::CJK_UI_FONT_HEIGHT;
    const int top = srcHeight - CJK_UI_FONT_DESCENT;
    renderBitmap(bitmap, srcWidth, srcHeight, CjkUiFont20::CJK_UI_FONT_BYTES_PER_ROW, top, top, 0, -1, true);
  }
}

int GfxRenderer::measureTextAdvance(const int resolvedFontId, const EpdFontFamily& fontFamily, const char* text,
                                    const EpdFontFamily::Style style) const {
  uint32_t cp;
  uint32_t prevCp = 0;
  bool prevWasEpd = false;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;
  const bool isSupSub = isHalfScaleStyle(style);

  auto sdIt = sdCardFonts_.find(resolvedFontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const uint8_t styleIdx = resolveSdCardStyle(*sdIt->second, style);
    const char* scan = text;
    uint32_t fastPrevCp = 0;
    int fastWidthPx = 0;
    int32_t fastPrevAdvanceFP = 0;
    bool complete = true;
    while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&scan)))) {
      if (BidiUtils::isTransparentMark(cp) || utf8IsCombiningMark(cp)) continue;
      cp = fontFamily.applyLigatures(cp, scan, style);
      if (!fontFamily.hasCodepoint(cp, style)) {
        complete = false;
        break;
      }

      const int32_t advanceFP = sdIt->second->getAdvance(cp, styleIdx);
      if (advanceFP == 0) {
        complete = false;
        break;
      }

      if (fastPrevCp != 0) {
        fastWidthPx +=
            snapTextAdvance(fastPrevAdvanceFP + fontFamily.getKerning(fastPrevCp, cp, style), isSupSub);
      }
      fastPrevAdvanceFP = advanceFP;
      fastPrevCp = cp;
    }
    if (complete) return fastWidthPx + snapTextAdvance(fastPrevAdvanceFP, isSupSub);
  }

  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (BidiUtils::isTransparentMark(cp) || utf8IsCombiningMark(cp)) continue;
    cp = fontFamily.applyLigatures(cp, text, style);

    const bool hasExactEpdGlyph = fontFamily.hasCodepoint(cp, style);
    const EpdGlyph* glyph = hasExactEpdGlyph ? fontFamily.getGlyph(cp, style) : nullptr;
    TextFallbackGlyph fallback;
    const bool hasFallback = !hasExactEpdGlyph && resolveTextFallback(resolvedFontId, cp, false, &fallback);
    if (!glyph && !hasFallback) glyph = fontFamily.getGlyph(cp, style);

    if (prevCp != 0) {
      const int32_t kernFP = prevWasEpd && glyph ? fontFamily.getKerning(prevCp, cp, style) : 0;
      widthPx += snapTextAdvance(prevAdvanceFP + kernFP, isSupSub);
    }

    if (glyph) {
      prevAdvanceFP = glyph->advanceX;
    } else {
      prevAdvanceFP = hasFallback ? fp4::fromPixel(fallback.advance) : 0;
    }
    prevWasEpd = glyph != nullptr;
    prevCp = cp;
  }

  return widthPx + snapTextAdvance(prevAdvanceFP, isSupSub);
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                              const BidiUtils::BidiBaseDir baseDir) const {
  if (text == nullptr || *text == '\0') return 0;

  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return 0;
  }
  const EpdFontFamily& fontFamily = fontIt->second;

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);

  bool needsFallback = false;
  const char* checkPtr = renderedText;
  uint32_t testCp;
  while ((testCp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&checkPtr)))) {
    if (BidiUtils::isTransparentMark(testCp) || utf8IsCombiningMark(testCp)) continue;
    testCp = fontFamily.applyLigatures(testCp, checkPtr, style);
    if (!fontFamily.hasCodepoint(testCp, style) || fontFamily.getGlyph(testCp, style) == nullptr) {
      needsFallback = true;
      break;
    }
  }

  if (needsFallback) {
    const auto widthStyle = static_cast<EpdFontFamily::Style>(
        static_cast<uint8_t>(style) & ~static_cast<uint8_t>(EpdFontFamily::SUP | EpdFontFamily::SUB));
    return measureTextAdvance(resolvedFontId, fontFamily, renderedText, widthStyle);
  }

  int w = 0;
  int h = 0;
  fontFamily.getTextDimensions(renderedText, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  const int x = (getScreenWidth() - getTextAdvanceX(fontId, text, style, baseDir)) / 2;
  drawText(fontId, x, y, text, black, style, baseDir);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  // Route CJK-bearing strings to the fallback font when the requested font
  // lacks the glyphs (e.g. Chinese book titles drawn with a Latin UI font).
  const int resolvedFontId = resolveTextFontId(fontId, text, style);

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);

  const int yPos = y + getFontAscenderSize(resolvedFontId);
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(renderedText, resolvedFontId, style);
    return;
  }

  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return;
  }
  const auto& font = fontIt->second;

  const char* textCursor = renderedText;
  uint32_t cp;
  uint32_t prevCp = 0;
  bool prevWasEpd = false;
  const bool isSupSub = isHalfScaleStyle(style);
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&textCursor)))) {
    // RTL vowel marks (Hebrew niqqud, Arabic harakat) ride the combining-mark
    // path: zero-advance overlays on the preceding base glyph (applyBidiVisual
    // emits base-then-marks per UAX#9 L3). anchorFor pins position-sensitive
    // niqqud (dagesh, shin/sin dots, holam) to their spot on the base; other
    // marks stay centered, raised above the base or (kasra) at their
    // font-native position. Fonts without their glyphs — the built-ins — miss
    // the getGlyph lookup and skip them, as before.
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      if (!font.hasCodepoint(cp, style)) continue;
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int combiningLeft = isSupSub ? combiningGlyph->left / 2 : combiningGlyph->left;
      const int combiningWidth = isSupSub ? (combiningGlyph->width + 1) / 2 : combiningGlyph->width;
      const int combiningTop = isSupSub ? combiningGlyph->top / 2 : combiningGlyph->top;
      const int combiningHeight = isSupSub ? (combiningGlyph->height + 1) / 2 : combiningGlyph->height;
      const int raiseBy = combiningMark::raiseAboveBase(anchor, combiningTop, combiningHeight, lastBaseTop);
      const int combiningX =
          combiningMark::anchorOver(anchor, lastBaseX, lastBaseLeft, lastBaseWidth, combiningLeft, combiningWidth);
      if (isSupSub) {
        renderCharScaled<TextRotation::None>(*this, renderMode, font, cp, combiningX, yPos - raiseBy, black, style);
      } else {
        renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, combiningX, yPos - raiseBy, black, style);
      }
      continue;
    }

    cp = font.applyLigatures(cp, textCursor, style);

    const bool hasExactEpdGlyph = font.hasCodepoint(cp, style);
    const EpdGlyph* glyph = hasExactEpdGlyph ? font.getGlyph(cp, style) : nullptr;
    TextFallbackGlyph fallback;
    const bool hasFallback = !hasExactEpdGlyph && resolveTextFallback(resolvedFontId, cp, true, &fallback);
    if (!glyph && !hasFallback) glyph = font.getGlyph(cp, style);

    // Kerning belongs to one EPD font. Do not carry it across an external or
    // built-in fallback boundary; the fallback advance is already authoritative.
    if (prevCp != 0) {
      const int32_t kernFP = prevWasEpd && glyph ? font.getKerning(prevCp, cp, style) : 0;
      lastBaseX += snapTextAdvance(prevAdvanceFP + kernFP, isSupSub);
    }

    const int fallbackWidth = hasFallback && fallback.metrics.width > 0 ? fallback.metrics.width : fallback.advance;
    lastBaseLeft = glyph ? (isSupSub ? glyph->left / 2 : glyph->left)
                         : (hasFallback ? (isSupSub ? fallback.metrics.left / 2 : fallback.metrics.left) : 0);
    lastBaseWidth = glyph ? (isSupSub ? (glyph->width + 1) / 2 : glyph->width)
                          : (hasFallback ? (isSupSub ? (fallbackWidth + 1) / 2 : fallbackWidth) : 0);
    const int fallbackTop =
        hasFallback ? (isSupSub ? fallback.metrics.top / 2 - fallback.baselineOffset / 2
                                : fallback.metrics.top - fallback.baselineOffset)
                    : 0;
    lastBaseTop = glyph ? (isSupSub ? glyph->top / 2 : glyph->top) : fallbackTop;

    if (glyph) {
      prevAdvanceFP = glyph->advanceX;
      if (isSupSub) {
        renderCharScaled<TextRotation::None>(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
      } else {
        renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
      }
    } else {
      prevAdvanceFP = hasFallback ? fp4::fromPixel(fallback.advance) : 0;
      if (hasFallback) {
        int fallbackX = lastBaseX;
        renderTextFallback(fallback, cp, &fallbackX, yPos, black, isSupSub);
      }
    }
    prevWasEpd = glyph != nullptr;
    prevCp = cp;
  }
}

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, const BidiUtils::BidiBaseDir baseDir) {
  if (!text || *text == '\0') return text;

  if (baseDir != BidiUtils::BidiBaseDir::RTL) {
    // Byte-level scan: skip BiDi when no RTL script lead bytes are present.
    // Hebrew UTF-8 lead bytes: 0xD6-0xD7; Arabic/Syriac: 0xD8-0xDB.
    // This covers all RTL content without false negatives and avoids triggering
    // the full UAX#9 algorithm for Latin-extended, em-dashes, accented text, etc.
    bool hasRtlBytes = false;
    for (const unsigned char* q = reinterpret_cast<const unsigned char*>(text); *q; ++q) {
      if (*q >= 0xD6 && *q <= 0xDB) {
        hasRtlBytes = true;
        break;
      }
    }
    if (!hasRtlBytes) return text;
  }

  if (BidiUtils::applyBidiVisual(text, visualBuffer, static_cast<int>(baseDir)) && !visualBuffer.empty()) {
    return visualBuffer.c_str();
  }
  return text;
}
}  // namespace

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, state);
    }
  } else if (y1 == y2) {
    if (x2 < x1) {
      std::swap(x1, x2);
    }
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, state);
    }
  } else {
    // Bresenham's line algorithm — integer arithmetic only
    int dx = x2 - x1;
    int dy = y2 - y1;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    dx = sx * dx;  // abs
    dy = sy * dy;  // abs

    int err = dx - dy;
    while (true) {
      drawPixel(x1, y1, state);
      if (x1 == x2 && y1 == y2) break;
      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x1 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y1 += sy;
      }
    }
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x1, y1 + i, x2, y2 + i, state);
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

// Border is inside the rectangle
void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x + i, y + i, x + width - i, y + i, state);
    drawLine(x + width - i, y + i, x + width - i, y + height - i, state);
    drawLine(x + width - i, y + height - i, x + i, y + height - i, state);
    drawLine(x + i, y + height - i, x + i, y + i, state);
  }
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadius = maxRadius;

  if (outerRadius <= 0) {
    return;
  }

  const int outerRadiusSq = outerRadius * outerRadius;
  const int innerRadiusSq = innerRadius * innerRadius;

  int xOuter = outerRadius;
  int xInner = innerRadius;

  for (int dy = 0; dy <= outerRadius; ++dy) {
    while (xOuter > 0 && (xOuter * xOuter + dy * dy) > outerRadiusSq) {
      --xOuter;
    }
    // Keep the smallest x that still lies outside/at the inner radius,
    // i.e. (x^2 + y^2) >= innerRadiusSq.
    while (xInner > 0 && ((xInner - 1) * (xInner - 1) + dy * dy) >= innerRadiusSq) {
      --xInner;
    }

    if (xOuter < xInner) {
      continue;
    }

    const int x0 = cx + xDir * xInner;
    const int x1 = cx + xDir * xOuter;
    const int left = std::min(x0, x1);
    const int width = std::abs(x1 - x0) + 1;
    const int py = cy + yDir * dy;

    if (width > 0) {
      fillRect(left, py, width, 1, state);
    }
  }
};

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  if (lineWidth <= 0 || width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, state);
    return;
  }

  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) {
      fillRect(x + maxRadius, y, horizontalWidth, stroke, state);
    }
    if (roundBottomLeft || roundBottomRight) {
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, state);
    }
  }

  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) {
      fillRect(x, y + maxRadius, stroke, verticalHeight, state);
    }
    if (roundTopRight || roundBottomRight) {
      fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, state);
    }
  }

  if (roundTopLeft) {
    drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, state);
  }
  if (roundTopRight) {
    drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, state);
  }
  if (roundBottomRight) {
    drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, state);
  }
  if (roundBottomLeft) {
    drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, state);
  }
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  if (state) {
    fillRectImpl<Color::Black>(x, y, width, height);
  } else {
    fillRectImpl<Color::White>(x, y, width, height);
  }
}

// NOTE: Those are in critical path, and need to be templated to avoid runtime checks for every pixel.
// Any branching must be done outside the loops to avoid performance degradation.
template <>
void GfxRenderer::drawPixelDither<Color::Clear>(const int x, const int y) const {
  // Do nothing
}

template <>
void GfxRenderer::drawPixelDither<Color::Black>(const int x, const int y) const {
  drawPixel(x, y, true);
}

template <>
void GfxRenderer::drawPixelDither<Color::White>(const int x, const int y) const {
  drawPixel(x, y, false);
}

template <>
void GfxRenderer::drawPixelDither<Color::LightGray>(const int x, const int y) const {
  drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
}

template <>
void GfxRenderer::drawPixelDither<Color::DarkGray>(const int x, const int y) const {
  drawPixel(x, y, (x + y) % 2 == 0);  // TODO: maybe find a better pattern?
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  switch (color) {
    case Color::Clear:
      break;
    case Color::Black:
      fillRectImpl<Color::Black>(x, y, width, height);
      break;
    case Color::White:
      fillRectImpl<Color::White>(x, y, width, height);
      break;
    case Color::LightGray:
      fillRectImpl<Color::LightGray>(x, y, width, height);
      break;
    case Color::DarkGray:
      fillRectImpl<Color::DarkGray>(x, y, width, height);
      break;
  }
}

template <Color C>
void GfxRenderer::fillRectImpl(const int x, const int y, const int width, const int height) const {
  if constexpr (C == Color::Clear) return;
  if (width <= 0 || height <= 0) return;
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  // Clip in logical space.
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const int lx0 = std::max(0, x);
  const int ly0 = std::max(0, y);
  const int lx1 = std::min(screenW, x + width);
  const int ly1 = std::min(screenH, y + height);
  if (lx0 >= lx1 || ly0 >= ly1) return;

  // Rotate the two opposing logical corners into physical-framebuffer space.
  // The bounding rect in physical space is the rect we need to fill — rotation
  // is rigid (no shear/stretch) so the bbox of the two corners IS the rect.
  int paX, paY, pbX, pbY;
  rotateCoordinates(orientation, lx0, ly0, &paX, &paY, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &pbX, &pbY, panelWidth, panelHeight);

  const int phyX0 = std::min(paX, pbX);
  const int phyX1 = std::max(paX, pbX);  // inclusive
  int phyY0 = std::min(paY, pbY);
  int phyY1 = std::max(paY, pbY);

  // Strip mode: clip Y range to the active band and redirect writes.
  uint8_t* target = getWriteTarget();
  const int originY = getWriteOriginY();
  const int writeRows = getWriteRows();
  phyY0 = std::max(phyY0, originY);
  phyY1 = std::min(phyY1, originY + writeRows - 1);
  if (phyY0 > phyY1) return;

  // Bit/byte layout: MSB-first within a byte, so phyX → bit (7 - (phyX & 7)).
  // Head and tail masks cover only the in-rect bits of the first/last byte.
  const int byteStart = phyX0 >> 3;
  const int byteEnd = phyX1 >> 3;  // inclusive
  const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (phyX0 & 7));
  const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (phyX1 & 7)));
  const int32_t panelStride = static_cast<int32_t>(panelWidthBytes);

  if constexpr (C == Color::Black || C == Color::White) {
    // Solid fill. Framebuffer: 0 = black, 1 = white.
    const uint8_t fillByte = (C == Color::Black) ? 0x00u : 0xFFu;
    for (int py = phyY0; py <= phyY1; ++py) {
      uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t mask = headMask & tailMask;
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~mask);
        } else {
          row[byteStart] |= mask;
        }
      } else {
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~headMask);
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] &= static_cast<uint8_t>(~tailMask);
        } else {
          row[byteStart] |= headMask;
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] |= tailMask;
        }
      }
    }
  } else {
    // Dither (LightGray / DarkGray). Both patterns have period 2 in logical
    // (x, y), so per physical row we precompute one byte that represents the
    // pattern across an 8-pixel stretch — every full byte in the row uses
    // that same value.
    //
    // dlxPerPhyX / dlyPerPhyX: how logical (x, y) change as phyX increments
    // along a physical row. Derived from inverting rotateCoordinates.
    int dlxPerPhyX = 0, dlyPerPhyX = 0;
    switch (orientation) {
      case Portrait:
        dlxPerPhyX = 0;
        dlyPerPhyX = 1;
        break;
      case PortraitInverted:
        dlxPerPhyX = 0;
        dlyPerPhyX = -1;
        break;
      case LandscapeClockwise:
        dlxPerPhyX = -1;
        dlyPerPhyX = 0;
        break;
      case LandscapeCounterClockwise:
        dlxPerPhyX = 1;
        dlyPerPhyX = 0;
        break;
    }

    // The dither pattern has period 2 in logical space, and each orientation
    // maps py to logical coords with a fixed parity relationship. The
    // blackMask byte therefore repeats with period 2 in py. Precompute both
    // variants outside the row loop to eliminate the per-row switch + 8-bit
    // construction loop.
    uint8_t blackMasks[2];
    for (int parityIdx = 0; parityIdx < 2; ++parityIdx) {
      const int samplePy = phyY0 + parityIdx;
      int lxBase = 0, lyBase = 0;
      switch (orientation) {
        case Portrait:
          lxBase = panelHeight - 1 - samplePy;
          lyBase = byteStart * 8;
          break;
        case PortraitInverted:
          lxBase = samplePy;
          lyBase = panelWidth - 1 - byteStart * 8;
          break;
        case LandscapeClockwise:
          lxBase = panelWidth - 1 - byteStart * 8;
          lyBase = panelHeight - 1 - samplePy;
          break;
        case LandscapeCounterClockwise:
          lxBase = byteStart * 8;
          lyBase = samplePy;
          break;
      }
      uint8_t mask = 0;
      for (int b = 0; b < 8; ++b) {
        const int lx = lxBase + b * dlxPerPhyX;
        const int ly = lyBase + b * dlyPerPhyX;
        bool isBlack;
        if constexpr (C == Color::LightGray) {
          isBlack = ((lx & 1) == 0) && ((ly & 1) == 0);
        } else {  // DarkGray
          isBlack = (((lx + ly) & 1) == 0);
        }
        if (isBlack) mask |= static_cast<uint8_t>(1u << (7 - b));
      }
      blackMasks[samplePy & 1] = mask;
    }

    for (int py = phyY0; py <= phyY1; ++py) {
      const uint8_t blackMask = blackMasks[py & 1];
      const uint8_t whiteMask = static_cast<uint8_t>(~blackMask);

      // Dither writes BOTH inks (the slow path called drawPixel for every
      // pixel — setting or clearing — so we must do the same). Inside the
      // rect mask: write whiteMask (1s where white, 0s where black). Outside
      // the rect mask: leave the framebuffer untouched.
      uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t rectMask = headMask & tailMask;
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~rectMask) | (rectMask & whiteMask));
      } else {
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~headMask) | (headMask & whiteMask));
        if (byteEnd > byteStart + 1) {
          // Period 2, so every full byte in this row is exactly whiteMask.
          memset(row + byteStart + 1, whiteMask, byteEnd - byteStart - 1);
        }
        row[byteEnd] = static_cast<uint8_t>((row[byteEnd] & ~tailMask) | (tailMask & whiteMask));
      }
    }
  }
}

template void GfxRenderer::fillRectImpl<Color::Black>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::White>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::LightGray>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::DarkGray>(int, int, int, int) const;

void GfxRenderer::maskRoundedRectOutsideCorners(const int x, const int y, const int width, const int height,
                                                const int radius, const Color color) const {
  if (radius <= 0 || color == Color::Clear) {
    return;
  }

  const int rr = radius - 1;
  const int rr2 = rr * rr;
  for (int dy = 0; dy < radius; dy++) {
    for (int dx = 0; dx < radius; dx++) {
      const int tx = rr - dx;
      const int ty = rr - dy;
      if (tx * tx + ty * ty > rr2) {
        if (color == Color::White || color == Color::Black) {
          bool state = color == Color::Black;
          drawPixel(x + dx, y + dy, state);                           // top-left
          drawPixel(x + width - 1 - dx, y + dy, state);               // top-right
          drawPixel(x + dx, y + height - 1 - dy, state);              // bottom-left
          drawPixel(x + width - 1 - dx, y + height - 1 - dy, state);  // bottom-right
        } else if (color == Color::LightGray) {
          drawPixelDither<Color::LightGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::LightGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        } else if (color == Color::DarkGray) {
          drawPixelDither<Color::DarkGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::DarkGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        }
      }
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  if (maxRadius <= 0) return;

  if constexpr (color == Color::Clear) {
    return;
  }

  const int radiusSq = maxRadius * maxRadius;

  // Avoid sqrt by scanning from outer radius inward while y grows.
  int x = maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    while (x > 0 && (x * x + dy * dy) > radiusSq) {
      --x;
    }
    if (x < 0) break;

    const int py = cy + yDir * dy;
    if (py < 0 || py >= getScreenHeight()) continue;

    int x0 = cx;
    int x1 = cx + xDir * x;
    if (x0 > x1) std::swap(x0, x1);
    const int width = x1 - x0 + 1;

    if (width <= 0) continue;

    if constexpr (color == Color::Black) {
      fillRect(x0, py, width, 1, true);
    } else if constexpr (color == Color::White) {
      fillRect(x0, py, width, 1, false);
    } else {
      // LightGray / DarkGray: use existing dithered fill path.
      fillRectDither(x0, py, width, 1, color);
    }
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  if (width <= 0 || height <= 0) {
    return;
  }

  // Assume if we're not rounding all corners then we are only rounding one side
  const int roundedSides = (!roundTopLeft || !roundTopRight || !roundBottomLeft || !roundBottomRight) ? 1 : 2;
  const int maxRadius = std::min({cornerRadius, width / roundedSides, height / roundedSides});
  if (maxRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    fillRectDither(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  }

  const int leftFillTop = y + (roundTopLeft ? (maxRadius + 1) : 0);
  const int leftFillBottom = y + height - 1 - (roundBottomLeft ? (maxRadius + 1) : 0);
  if (leftFillBottom >= leftFillTop) {
    fillRectDither(x, leftFillTop, maxRadius + 1, leftFillBottom - leftFillTop + 1, color);
  }

  const int rightFillTop = y + (roundTopRight ? (maxRadius + 1) : 0);
  const int rightFillBottom = y + height - 1 - (roundBottomRight ? (maxRadius + 1) : 0);
  if (rightFillBottom >= rightFillTop) {
    fillRectDither(x + width - maxRadius - 1, rightFillTop, maxRadius + 1, rightFillBottom - rightFillTop + 1, color);
  }

  auto fillArcTemplated = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    switch (color) {
      case Color::Clear:
        break;
      case Color::Black:
        fillArc<Color::Black>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::White:
        fillArc<Color::White>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::LightGray:
        fillArc<Color::LightGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::DarkGray:
        fillArc<Color::DarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
    }
  };

  if (roundTopLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  }

  if (roundTopRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  }

  if (roundBottomRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  }

  if (roundBottomLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY, panelWidth, panelHeight);
  // Rotate origin corner
  switch (orientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }
  // TODO: Rotate bits
  display.drawImage(bitmap, rotatedX, rotatedY, width, height);

  // In dark mode, invert the drawn region so icons adapt to dark background.
  // drawImage bypasses drawPixel (raw memcpy), so we invert the bytes post-hoc.
  if (darkMode && renderMode == BW) {
    const int widthBytes = width / 8;
    for (int row = 0; row < height; row++) {
      const int bufY = rotatedY + row;
      if (bufY < 0 || bufY >= HalDisplay::DISPLAY_HEIGHT) continue;
      const int bufStart = bufY * HalDisplay::DISPLAY_WIDTH_BYTES + (rotatedX / 8);
      for (int b = 0; b < widthBytes; b++) {
        const int idx = bufStart + b;
        if (idx >= 0 && idx < HalDisplay::BUFFER_SIZE) {
          frameBuffer[idx] = ~frameBuffer[idx];
        }
      }
    }
  }
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int size) const {
  // Plot the icon pixel-by-pixel through drawPixel (which applies the orientation
  // transform) instead of the byte-aligned framebuffer blit. The blit snaps the
  // icon's position to 8px (one byte) along the rotated axis, which prevents it
  // from aligning with adjacent text; per-pixel plotting is pixel-precise.
  // Icons are square and 1bpp (MSB-first, bit==0 = ink). The (size-1-row, col)
  // mapping reproduces the Portrait orientation the blit produced; drawIcon is
  // only called by the UI themes, which all render in forced Portrait.
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) {
        drawPixel(x + (size - 1 - row), y + col, true);
      }
    }
  }
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  // Cover art and images should keep their original colors in dark mode
  skipDarkModeForImages = true;
  auto cleanup = [this]() { skipDarkModeForImages = false; };

  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
    cleanup();
    return;
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);
  LOG_DBG("GFX", "Cropping %dx%d by %dx%d pix, is %s", bitmap.getWidth(), bitmap.getHeight(), cropPixX, cropPixY,
          bitmap.isTopDown() ? "top-down" : "bottom-up");

  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  bool hasTargetBounds = false;
  float fitScale = 1.0f;

  if (maxWidth > 0 && croppedWidth > 0.0f) {
    fitScale = static_cast<float>(maxWidth) / croppedWidth;
    hasTargetBounds = true;
  }

  if (maxHeight > 0 && croppedHeight > 0.0f) {
    const float heightScale = static_cast<float>(maxHeight) / croppedHeight;
    fitScale = hasTargetBounds ? std::min(fitScale, heightScale) : heightScale;
    hasTargetBounds = true;
  }

  if (hasTargetBounds && fitScale < 1.0f) {
    scale = fitScale;
    isScaled = true;
  }
  LOG_DBG("GFX", "Scaling by %f - %s", scale, isScaled ? "scaled" : "not scaled");

  if (darkMode && renderMode == BW) {
    const int sourceWidth = static_cast<int>((1.0f - cropX) * bitmap.getWidth() - cropPixX);
    const int sourceHeight = static_cast<int>((1.0f - cropY) * bitmap.getHeight() - cropPixY);
    const int scaledWidth = isScaled ? static_cast<int>(std::floor(sourceWidth * scale)) : sourceWidth;
    const int scaledHeight = isScaled ? static_cast<int>(std::floor(sourceHeight * scale)) : sourceHeight;
    fillRect(x, y, scaledWidth, scaledHeight, false);
  }

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  BitmapRowBuffers buffers(outputRowSize, bitmap.getRowBytes());
  if (!buffers.ok()) {
    LOG_ERR("GFX", "!! Failed to allocate BMP row buffers");
    return;
  }

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }

    if (bitmap.readNextRow(buffers.outputRow, buffers.rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);
      return;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      const uint8_t val = buffers.outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (renderMode == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
        drawPixel(screenX, screenY, false);
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        drawPixel(screenX, screenY, false);
      }
    }
  }

  cleanup();
}

void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  // Cover art and images should keep their original colors in dark mode
  skipDarkModeForImages = true;
  auto cleanup = [this]() { skipDarkModeForImages = false; };

  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  // In dark mode, pre-fill image area with white since white pixels are
  // not explicitly drawn (they rely on clearScreen background).
  if (darkMode && renderMode == BW) {
    const int displayWidth = isScaled ? static_cast<int>(std::floor(bitmap.getWidth() * scale)) : bitmap.getWidth();
    const int displayHeight = isScaled ? static_cast<int>(std::floor(bitmap.getHeight() * scale)) : bitmap.getHeight();
    fillRect(x, y, displayWidth, displayHeight, false);
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  BitmapRowBuffers buffers(outputRowSize, bitmap.getRowBytes());
  if (!buffers.ok()) {
    LOG_ERR("GFX", "!! Failed to allocate 1-bit BMP row buffers");
    return;
  }

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(buffers.outputRow, buffers.rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);
      return;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY = y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale)) : bmpYOffset);
    if (screenY >= getScreenHeight()) {
      continue;  // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      // Get 2-bit value (result of readNextRow quantization)
      const uint8_t val = buffers.outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      // For 1-bit source: 0 or 1 -> map to black (0,1,2) or white (3)
      // val < 3 means black pixel (draw it)
      if (val < 3) {
        drawPixel(screenX, screenY, true);
      }
      // White pixels (val == 3) are not drawn (leave background)
    }
  }

  cleanup();
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto* nodeX = static_cast<int*>(malloc(numPoints * sizeof(int)));
  if (!nodeX) {
    LOG_ERR("GFX", "!! Failed to allocate polygon node buffer");
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X (simple bubble sort, numPoints is small)
    for (int i = 0; i < nodes - 1; i++) {
      for (int k = i + 1; k < nodes; k++) {
        if (nodeX[i] > nodeX[k]) {
          int temp = nodeX[i];
          nodeX[i] = nodeX[k];
          nodeX[k] = temp;
        }
      }
    }

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }

  free(nodeX);
}

// For performance measurement (using static to allow "const" methods)
static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms = millis();
  // Dark mode: invert the default white (0xFF) background to black (0x00).
  // Grayscale prep passes clearScreen(0x00) and is not affected.
  display.clearScreen((darkMode && color == 0xFF) ? 0x00 : color);
}

void GfxRenderer::beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const {
  // Band is caller-guaranteed in-bounds (the reader's grayscale loop computes
  // it); assert catches future misuse in debug before it mis-renders or wraps
  // the downstream uint16_t cast in writeGrayscalePlaneStrip.
  assert(scratch != nullptr && stripRows > 0 && stripY0 >= 0 && stripY0 <= static_cast<int>(panelHeight) - stripRows);
  _stripBuf = scratch;
  _stripY0 = stripY0;
  _stripRows = stripRows;
  _stripActive = true;
}

void GfxRenderer::endStripTarget() const {
  _stripActive = false;
  _stripBuf = nullptr;
  _stripY0 = 0;
  _stripRows = 0;
}

bool GfxRenderer::glyphIntersectsStrip(int x0, int y0, int x1, int y1) const {
  if (!_stripActive) {
    return true;
  }
  // Rotate the two opposite bbox corners to physical coords. For 90-degree
  // orientations the physical bbox stays axis-aligned, so min/max of the two
  // rotated corners' Y bounds the glyph's physical y-extent.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x0, y0, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x1, y1, &bx, &by, panelWidth, panelHeight);
  const int minY = ay < by ? ay : by;
  const int maxY = ay > by ? ay : by;
  return !(maxY < _stripY0 || minY >= _stripY0 + _stripRows);
}
void GfxRenderer::invertScreen() const {
  for (uint32_t i = 0; i < frameBufferSize; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::setPartialUpdateRect(int x, int y, int width, int height) const {
  partialX_ = x;
  partialY_ = y;
  partialW_ = width;
  partialH_ = height;
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode) const {
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);

  const bool hasPartialUpdate = partialW_ > 0 && partialH_ > 0;
  if (hasPartialUpdate) {
    PhysicalRect physicalWindow;
    const bool hasPhysicalWindow =
        refreshMode == HalDisplay::FAST_REFRESH &&
        logicalRectToPhysicalWindow(orientation, partialX_, partialY_, partialW_, partialH_, getScreenWidth(),
                                    getScreenHeight(), panelWidth, panelHeight, &physicalWindow);
    partialX_ = partialY_ = partialW_ = partialH_ = 0;
    if (hasPhysicalWindow) {
      if (darkMode) {
        display.displayWindowDarkRedrive(physicalWindow.x, physicalWindow.y, physicalWindow.width,
                                         physicalWindow.height, fadingFix);
      } else {
        display.displayWindow(physicalWindow.x, physicalWindow.y, physicalWindow.width, physicalWindow.height,
                              fadingFix);
      }
      return;
    }
  }

  if (darkMode && (refreshMode == HalDisplay::FAST_REFRESH || refreshMode == HalDisplay::HALF_REFRESH)) {
    display.displayBuffer(HalDisplay::DARK_REDRIVE, fadingFix);
    return;
  }

  display.displayBuffer(refreshMode, fadingFix);
}

void GfxRenderer::displayBufferAsync(const HalDisplay::RefreshMode refreshMode) const {
  // The async path has no turn-off-screen hook, which the sunlight fading fix
  // relies on. Dark-mode FAST/HALF refreshes also have to pass through displayBuffer()
  // so they become a DARK_REDRIVE rather than whitening the background.
  const bool darkModeNeedsRedrive =
      darkMode && (refreshMode == HalDisplay::FAST_REFRESH || refreshMode == HalDisplay::HALF_REFRESH);
  if (fadingFix || darkModeNeedsRedrive) {
    displayBuffer(refreshMode);
    return;
  }
  display.displayBufferAsync(refreshMode);
}

void GfxRenderer::waitRefreshComplete() const { display.waitRefreshComplete(); }

bool GfxRenderer::supportsAsyncRefresh() const { return !fadingFix && display.supportsAsyncRefresh(); }

size_t GfxRenderer::readFramebufferRegion(int x, int y, int w, int h, uint8_t* dst, size_t dstCapacity) const {
  if (dst == nullptr || w <= 0 || h <= 0) return 0;

  const AlignedMemRect mem = screenRectToAlignedMemRect(orientation, x, y, w, h, panelWidth, panelHeight);
  if (!mem.valid) return 0;

  const size_t rowBytes = mem.w / 8;  // exact: mem.w is a multiple of 8
  const size_t needed = rowBytes * mem.h;
  if (needed > dstCapacity) return 0;

  for (uint16_t row = 0; row < mem.h; ++row) {
    const uint8_t* srcRow = frameBuffer + (static_cast<uint32_t>(mem.y + row) * panelWidthBytes) + (mem.x / 8);
    uint8_t* dstRow = dst + (static_cast<size_t>(row) * rowBytes);
    memcpy(dstRow, srcRow, rowBytes);
  }
  return needed;
}

void GfxRenderer::writeFramebufferRegion(int x, int y, int w, int h, const uint8_t* src) {
  if (src == nullptr || w <= 0 || h <= 0) return;

  const AlignedMemRect mem = screenRectToAlignedMemRect(orientation, x, y, w, h, panelWidth, panelHeight);
  if (!mem.valid) return;

  const size_t rowBytes = mem.w / 8;  // exact: mem.w is a multiple of 8

  for (uint16_t row = 0; row < mem.h; ++row) {
    const uint8_t* srcRow = src + (static_cast<size_t>(row) * rowBytes);
    uint8_t* dstRow = frameBuffer + (static_cast<uint32_t>(mem.y + row) * panelWidthBytes) + (mem.x / 8);
    memcpy(dstRow, srcRow, rowBytes);
  }
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  // U+2026 HORIZONTAL ELLIPSIS (UTF-8: 0xE2 0x80 0xA6)
  const char* ellipsis = "\xe2\x80\xa6";
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    // Text fits, return as is
    return item;
  }

  while (!item.empty() && getTextWidth(fontId, (item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }

  return item.empty() ? ellipsis : item + ellipsis;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  std::string remaining = text;
  std::string currentLine;

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncatedText fit it with an ellipsis.
      std::string lastContent = currentLine.empty() ? remaining : currentLine + " " + remaining;
      lines.push_back(truncatedText(fontId, lastContent.c_str(), maxWidth, style));
      return lines;
    }

    // Find next word
    size_t spacePos = remaining.find(' ');
    std::string word;

    if (spacePos == std::string::npos) {
      word = remaining;
      remaining.clear();
    } else {
      word = remaining.substr(0, spacePos);
      remaining.erase(0, spacePos + 1);
    }

    std::string testLine = currentLine.empty() ? word : currentLine + " " + word;

    if (getTextWidth(fontId, testLine.c_str(), style) <= maxWidth) {
      currentLine = testLine;
    } else {
      if (!currentLine.empty()) {
        lines.push_back(currentLine);
        // If the carried-over word itself exceeds maxWidth, truncate it and
        // push it as a complete line immediately — storing it in currentLine
        // would allow a subsequent short word to be appended after the ellipsis.
        if (getTextWidth(fontId, word.c_str(), style) > maxWidth) {
          lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine = word;
        }
      } else {
        // Single word wider than maxWidth: truncate and stop to avoid complicated
        // splitting rules (different between languages). Results in an aesthetically
        // pleasing end.
        lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
        return lines;
      }
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return panelHeight;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return panelWidth;
  }
  return panelHeight;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return panelWidth;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return panelHeight;
  }
  return panelWidth;
}

void GfxRenderer::tapToLogical(float nx, float ny, int& outX, int& outY) const {
  int phyX = static_cast<int>(nx * panelWidth);
  int phyY = static_cast<int>(ny * panelHeight);
  if (phyX < 0) phyX = 0;
  if (phyX > panelWidth - 1) phyX = panelWidth - 1;
  if (phyY < 0) phyY = 0;
  if (phyY > panelHeight - 1) phyY = panelHeight - 1;

  switch (orientation) {
    case Portrait:
      outX = panelHeight - 1 - phyY;
      outY = phyX;
      break;
    case PortraitInverted:
      outX = phyY;
      outY = panelWidth - 1 - phyX;
      break;
    case LandscapeClockwise:
      outX = panelWidth - 1 - phyX;
      outY = panelHeight - 1 - phyY;
      break;
    case LandscapeCounterClockwise:
    default:
      outX = phyX;
      outY = phyY;
      break;
  }
}

// Translate a logical rect through rotateCoordinates and take the bounding
// box of its four corners on the physical panel. Output coords are inclusive
// and clamped. Returns false if the rect ends up fully off-panel.
static bool logicalRectToPhysicalBounds(GfxRenderer::Orientation orientation, int lx, int ly, int lw, int lh,
                                        uint16_t panelWidth, uint16_t panelHeight, int* outX0, int* outY0, int* outX1,
                                        int* outY1) {
  if (lw <= 0 || lh <= 0) return false;
  int minX = INT32_MAX;
  int minY = INT32_MAX;
  int maxX = INT32_MIN;
  int maxY = INT32_MIN;
  const int corners[4][2] = {{lx, ly}, {lx + lw - 1, ly}, {lx, ly + lh - 1}, {lx + lw - 1, ly + lh - 1}};
  for (auto& c : corners) {
    int phyX;
    int phyY;
    rotateCoordinates(orientation, c[0], c[1], &phyX, &phyY, panelWidth, panelHeight);
    if (phyX < minX) minX = phyX;
    if (phyY < minY) minY = phyY;
    if (phyX > maxX) maxX = phyX;
    if (phyY > maxY) maxY = phyY;
  }
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  if (maxX >= panelWidth) maxX = panelWidth - 1;
  if (maxY >= panelHeight) maxY = panelHeight - 1;
  if (minX > maxX || minY > maxY) return false;
  *outX0 = minX;
  *outY0 = minY;
  *outX1 = maxX;
  *outY1 = maxY;
  return true;
}

size_t GfxRenderer::getRegionByteSize(int lx, int ly, int lw, int lh) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return 0;
  }
  // x bounds are in pixels; widen to byte boundaries on either side so per-row
  // memcpy stays byte-aligned even when the logical rect doesn't.
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  return static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
}

bool GfxRenderer::copyRegionToBuffer(int lx, int ly, int lw, int lh, uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    const uint8_t* src = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(buf + row * bytesPerRow, src, bytesPerRow);
  }
  return true;
}

bool GfxRenderer::copyBufferToRegion(int lx, int ly, int lw, int lh, const uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    uint8_t* dst = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(dst, buf + row * bytesPerRow, bytesPerRow);
  }
  return true;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  const bool isSupSub = isHalfScaleStyle(style);
  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      return scalePositiveTextAdvance(10, isSupSub);  // Default space width for 20px UI font
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return 0;
  }

  // Use external font's space advance when active - keeps word/space metrics consistent
  if (isReaderFont(fontId)) {
    FontManager& fm = FontManager::getInstance();
    if (fm.isExternalFontEnabled()) {
      ExternalFont* extFont = fm.getActiveFont();
      ExternalGlyphMetrics metrics{};
      if (extFont && extFont->getGlyphMetricsForLayout(' ', &metrics)) {
        const int advance = getExternalGlyphAdvanceForRendering(metrics, extFont->getCharWidth(), 0,
                                                                shouldUseCjkSymbolCellMetrics(' '),
                                                                shouldUseGlyphBoundsForAdvance(' '));
        return scalePositiveTextAdvance(advance, isSupSub);
      }
    }
  }

  const EpdGlyph* spaceGlyph = fontMap.at(effectiveFontId).getGlyph(' ', style);
  return spaceGlyph ? snapTextAdvance(spaceGlyph->advanceX, isSupSub) : 0;
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  const bool isSupSub = isHalfScaleStyle(style);
  const int effectiveFontId = getEffectiveFontId(fontId);
  const auto fontIt = fontMap.find(effectiveFontId);
  if (fontIt == fontMap.end()) return isUiFont(fontId) ? scalePositiveTextAdvance(10, isSupSub) : 0;

  const auto& font = fontIt->second;
  int32_t spaceAdvanceFP = 0;

  // External reader fonts may provide the displayed word glyphs while the built-in
  // fallback supplies missing characters. Keep natural space gaps consistent with
  // getTextAdvanceX()/getSpaceWidth() so disabling excessive justification does
  // not collapse English words together.
  const bool externalReaderFont = isReaderFont(fontId) && FontManager::getInstance().isExternalFontEnabled();
  if (externalReaderFont) {
    ExternalFont* extFont = FontManager::getInstance().getActiveFont();
    ExternalGlyphMetrics metrics{};
    if (extFont && extFont->getGlyphMetricsForLayout(' ', &metrics)) {
      const int advance = getExternalGlyphAdvanceForRendering(
          metrics, extFont->getCharWidth(), 0, shouldUseCjkSymbolCellMetrics(' '), shouldUseGlyphBoundsForAdvance(' '));
      return scalePositiveTextAdvance(advance, isSupSub);
    }
  }

  const EpdGlyph* spaceGlyph = font.getGlyph(' ', style);
  spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;

  // External reader fonts do not apply built-in kerning during glyph advance.
  // If the external space is missing and we fall back to the built-in space width,
  // keep the gap unkerned so layout stays consistent with getTextAdvanceX().
  if (externalReaderFont) {
    return snapTextAdvance(spaceAdvanceFP, isSupSub);
  }

  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return snapTextAdvance(spaceAdvanceFP + kernFP, isSupSub);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;

  const auto& font = fontIt->second;
  if (!font.hasCodepoint(leftCp, style) || !font.hasCodepoint(rightCp, style)) return 0;

  const EpdGlyph* leftGlyph = font.getGlyph(leftCp, style);
  if (!leftGlyph || !font.getGlyph(rightCp, style)) return 0;

  const int32_t kernFP = font.getKerning(leftCp, rightCp, style);
  const bool isSupSub = isHalfScaleStyle(style);
  return snapTextAdvance(static_cast<int32_t>(leftGlyph->advanceX) + kernFP, isSupSub) -
         snapTextAdvance(leftGlyph->advanceX, isSupSub);
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, const EpdFontFamily::Style style,
                                 const BidiUtils::BidiBaseDir baseDir) const {
  if (text == nullptr || *text == '\0') return 0;

  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return 0;
  }

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);
  return measureTextAdvance(resolvedFontId, fontIt->second, renderedText, style);
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  // Check if using external font for reader fonts
  if (isReaderFont(fontId)) {
    FontManager& fm = FontManager::getInstance();
    if (fm.isExternalFontEnabled()) {
      ExternalFont* extFont = fm.getActiveFont();
      if (extFont) {
        if (extFont->isRichMetricsFormat() && extFont->getAscender() > 0) {
          return extFont->getAscender();
        }
        return extFont->getCharHeight();
      }
    }
  }

  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      // Check external UI font first
      FontManager& fm = FontManager::getInstance();
      if (fm.isUiFontEnabled()) {
        ExternalFont* extFont = fm.getActiveUiFont();
        if (extFont && extFont->isLoaded()) {
          if (extFont->isRichMetricsFormat() && extFont->getAscender() > 0) {
            return extFont->getAscender();
          }
          return extFont->getCharHeight();
        }
      }
      // Built-in CJK font renders with 4px descent, so ascender = height - 4
      return CjkUiFont20::CJK_UI_FONT_HEIGHT - 4;
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return 0;
  }

  // For UI fonts that also have an EPD font in fontMap (UI_10, UI_12, SMALL_FONT),
  // the EPD font ascender must be used because the EPD rendering path uses
  // fontMap.at(effectiveFontId) for glyphs not in CjkUiFont20.
  // Using the CJK ascender (16px) here would misalign EPD glyphs.
  return fontMap.at(effectiveFontId).getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  // Check if using external font for reader fonts
  if (isReaderFont(fontId)) {
    FontManager& fm = FontManager::getInstance();
    if (fm.isExternalFontEnabled()) {
      ExternalFont* extFont = fm.getActiveFont();
      if (extFont) {
        if (extFont->isRichMetricsFormat() && extFont->getLineHeight() > 0) {
          return extFont->getLineHeight();
        }
        return extFont->getCharHeight();
      }
    }
  }

  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      // Check external UI font first
      FontManager& fm = FontManager::getInstance();
      if (fm.isUiFontEnabled()) {
        ExternalFont* extFont = fm.getActiveUiFont();
        if (extFont && extFont->isLoaded()) {
          if (extFont->isRichMetricsFormat() && extFont->getLineHeight() > 0) {
            return extFont->getLineHeight();
          }
          return extFont->getCharHeight() + 4;  // height + spacing
        }
      }
      return CjkUiFont20::CJK_UI_FONT_HEIGHT + 4;  // 20px + 4px spacing
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return 0;
  }

  return fontMap.at(effectiveFontId).getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getLineHeight(const int fontId, const float compression) const {
  return static_cast<int>(getLineHeight(fontId) * compression + 0.5f);
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      // Check external UI font first
      FontManager& fm = FontManager::getInstance();
      if (fm.isUiFontEnabled()) {
        ExternalFont* extFont = fm.getActiveUiFont();
        if (extFont && extFont->isLoaded()) {
          return extFont->getCharHeight();
        }
      }
      return CjkUiFont20::CJK_UI_FONT_HEIGHT;  // 20px for CJK UI font
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return 0;
  }
  return fontMap.at(effectiveFontId).getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') return;

  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return;
  }

  const auto& font = fontIt->second;
  const bool isSupSub = isHalfScaleStyle(style);
  int lastBaseY = y;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;
  uint32_t prevCp = 0;
  bool prevWasEpd = false;

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      if (!font.hasCodepoint(cp, style)) continue;
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;

      const auto anchor = combiningMark::anchorFor(cp);
      const int combiningLeft = isSupSub ? combiningGlyph->left / 2 : combiningGlyph->left;
      const int combiningWidth = isSupSub ? (combiningGlyph->width + 1) / 2 : combiningGlyph->width;
      const int combiningTop = isSupSub ? combiningGlyph->top / 2 : combiningGlyph->top;
      const int combiningHeight = isSupSub ? (combiningGlyph->height + 1) / 2 : combiningGlyph->height;
      const int raiseBy = combiningMark::raiseAboveBase(anchor, combiningTop, combiningHeight, lastBaseTop);
      const int combiningX = x - raiseBy;
      const int combiningY = combiningMark::anchorOverRotated90CW(anchor, lastBaseY, lastBaseLeft, lastBaseWidth,
                                                                  combiningLeft, combiningWidth);
      if (isSupSub) {
        renderCharScaled<TextRotation::Rotated90CW>(*this, renderMode, font, cp, combiningX, combiningY, black, style);
      } else {
        renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, combiningX, combiningY, black, style);
      }
      continue;
    }

    cp = font.applyLigatures(cp, text, style);

    const bool hasExactEpdGlyph = font.hasCodepoint(cp, style);
    const EpdGlyph* glyph = hasExactEpdGlyph ? font.getGlyph(cp, style) : nullptr;
    TextFallbackGlyph fallback;
    const bool hasFallback = !hasExactEpdGlyph && resolveTextFallback(resolvedFontId, cp, true, &fallback);
    if (!glyph && !hasFallback) glyph = font.getGlyph(cp, style);

    if (prevCp != 0) {
      const int32_t kernFP = prevWasEpd && glyph ? font.getKerning(prevCp, cp, style) : 0;
      lastBaseY -= snapTextAdvance(prevAdvanceFP + kernFP, isSupSub);
    }

    const int fallbackWidth = hasFallback && fallback.metrics.width > 0 ? fallback.metrics.width : fallback.advance;
    lastBaseLeft = glyph ? (isSupSub ? glyph->left / 2 : glyph->left)
                         : (hasFallback ? (isSupSub ? fallback.metrics.left / 2 : fallback.metrics.left) : 0);
    lastBaseWidth = glyph ? (isSupSub ? (glyph->width + 1) / 2 : glyph->width)
                          : (hasFallback ? (isSupSub ? (fallbackWidth + 1) / 2 : fallbackWidth) : 0);
    int fallbackTop = 0;
    if (hasFallback) {
      const int scale = isSupSub ? 2 : 1;
      if (fallback.kind == TextFallbackGlyph::Kind::External && fallback.font) {
        const EpdFontData* fontData = font.getData(style);
        const int epdAscender = fontData ? fontData->ascender : 0;
        const int fallbackAscender = getExternalFontAscenderForRendering(*fallback.font) + fallback.baselineOffset;
        fallbackTop = epdAscender / scale - fallbackAscender / scale + fallback.metrics.top / scale;
      } else if (fallback.kind == TextFallbackGlyph::Kind::BuiltinCjk) {
        const EpdFontData* fontData = font.getData(style);
        fallbackTop = fontData ? fontData->ascender / scale : fallback.metrics.top / scale;
      }
    }
    lastBaseTop = glyph ? (isSupSub ? glyph->top / 2 : glyph->top) : fallbackTop;

    if (glyph) {
      prevAdvanceFP = glyph->advanceX;
      if (isSupSub) {
        renderCharScaled<TextRotation::Rotated90CW>(*this, renderMode, font, cp, x, lastBaseY, black, style);
      } else {
        renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, x, lastBaseY, black, style);
      }
    } else {
      prevAdvanceFP = hasFallback ? fp4::fromPixel(fallback.advance) : 0;
      if (hasFallback) {
        renderTextFallbackRotated90CW(fallback, cp, x, lastBaseY, black, isSupSub);
      }
    }
    prevWasEpd = glyph != nullptr;
    prevCp = cp;
  }
}

void GfxRenderer::drawButtonHints(const int fontId, const char* btn1, const char* btn2, const char* btn3,
                                  const char* btn4) {
  const Orientation orientation = getOrientation();
  const int pageHeight = getScreenHeight();
  const int pageWidth = getScreenWidth();
  constexpr int buttonWidth = BUTTON_HINT_WIDTH;
  constexpr int buttonHeight = BUTTON_HINT_HEIGHT;
  constexpr int buttonY = BUTTON_HINT_BOTTOM_INSET;  // Distance from bottom
  constexpr int textYOffset = BUTTON_HINT_TEXT_OFFSET;
  constexpr int buttonPositions[] = {25, 130, 245, 350};
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const bool isLandscape =
      orientation == Orientation::LandscapeClockwise || orientation == Orientation::LandscapeCounterClockwise;
  const bool placeAtTop = orientation == Orientation::PortraitInverted;
  const int buttonTop = placeAtTop ? 0 : pageHeight - buttonY;

  if (isLandscape) {
    const bool placeLeft = orientation == Orientation::LandscapeClockwise;
    const int buttonLeft = placeLeft ? 0 : pageWidth - buttonWidth;
    if (orientation == Orientation::LandscapeCounterClockwise) {
      const char* tmp = labels[0];
      labels[0] = labels[3];
      labels[3] = tmp;
      tmp = labels[1];
      labels[1] = labels[2];
      labels[2] = tmp;
    }
    for (int i = 0; i < 4; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = buttonPositions[i];
        fillRect(buttonLeft, y, buttonWidth, buttonHeight, false);
        drawRect(buttonLeft, y, buttonWidth, buttonHeight);
        const int textWidth = getTextWidth(fontId, labels[i]);
        const int textX = buttonLeft + (buttonWidth - 1 - textWidth) / 2;
        drawText(fontId, textX, y + textYOffset, labels[i]);
      }
    }
    return;
  }

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      fillRect(x, buttonTop, buttonWidth, buttonHeight, false);
      drawRect(x, buttonTop, buttonWidth, buttonHeight);
      const int textWidth = getTextWidth(fontId, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      drawText(fontId, textX, buttonTop + textYOffset, labels[i]);
    }
  }
}

void GfxRenderer::drawSideButtonHints(const int fontId, const char* topBtn, const char* bottomBtn) const {
  const Orientation orientation = getOrientation();
  const int screenWidth = getScreenWidth();
  constexpr int buttonWidth = 40;   // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;  // Height on screen (width when rotated)
  constexpr int buttonX = 5;        // Distance from right edge
  // Position for the button group - buttons share a border so they're adjacent
  constexpr int topButtonY = 345;  // Top button position

  const char* labels[] = {topBtn, bottomBtn};

  // Draw the shared border for both buttons as one unit
  const bool placeLeft = orientation == Orientation::PortraitInverted;
  const int x = placeLeft ? buttonX : screenWidth - buttonX - buttonWidth;
  const int y = topButtonY;

  // Draw top button outline (3 sides, bottom open)
  if (topBtn != nullptr && topBtn[0] != '\0') {
    drawLine(x, y, x + buttonWidth - 1, y);   // Top
    drawLine(x, y, x, y + buttonHeight - 1);  // Left
    drawLine(x + buttonWidth - 1, y, x + buttonWidth - 1,
             y + buttonHeight - 1);  // Right
  }

  // Draw shared middle border
  if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
    drawLine(x, y + buttonHeight, x + buttonWidth - 1,
             y + buttonHeight);  // Shared border
  }

  // Draw bottom button outline (3 sides, top is shared)
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    drawLine(x, y + buttonHeight, x,
             y + 2 * buttonHeight - 1);  // Left
    drawLine(x + buttonWidth - 1, y + buttonHeight, x + buttonWidth - 1,
             y + 2 * buttonHeight - 1);  // Right
    drawLine(x, y + 2 * buttonHeight - 1, x + buttonWidth - 1,
             y + 2 * buttonHeight - 1);  // Bottom
  }

  // Draw text for each button
  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int yPos = y + i * buttonHeight;
      const char* label = labels[i];

      // Draw rotated text centered in the button
      const int textWidth = getTextWidth(fontId, label);
      int textHeight = getTextHeight(fontId);
      bool hasCjk = false;
      const char* scan = label;
      uint32_t cp;
      while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&scan)))) {
        if (isCjkCodepoint(cp)) {
          hasCjk = true;
          break;
        }
      }
      if (hasCjk) {
        textHeight = CjkUiFont20::CJK_UI_FONT_HEIGHT;
      }

      // Center the rotated text in the button
      const int textX = x + (buttonWidth - textHeight) / 2;
      const int textY = yPos + (buttonHeight + textWidth) / 2;

      drawTextRotated90CW(fontId, textX, textY, label);
    }
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::displayGrayscaleBase(HalDisplay::RefreshMode fallback) const {
  display.displayGrayscaleBase(fallback, fadingFix);
}

void GfxRenderer::preconditionGrayscale() const { display.preconditionGrayscale(); }

void GfxRenderer::preconditionGrayscale(int x, int y, int w, int h) const {
  if (w <= 0 || h <= 0) return;
  // Rotate the logical rect's opposite corners to physical panel coords; the
  // physical bbox stays axis-aligned for all four orientations.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x, y, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x + w - 1, y + h - 1, &bx, &by, panelWidth, panelHeight);
  int x0 = ax < bx ? ax : bx, x1 = ax > bx ? ax : bx;
  int y0 = ay < by ? ay : by, y1 = ay > by ? ay : by;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= panelWidth) x1 = panelWidth - 1;
  if (y1 >= panelHeight) y1 = panelHeight - 1;
  if (x1 < x0 || y1 < y0) return;
  display.preconditionGrayscale(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0),
                                static_cast<uint16_t>(x1 - x0 + 1), static_cast<uint16_t>(y1 - y0 + 1));
}

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer(const bool turnOffScreen, const bool darkMode) const {
  // Note: HalDisplay::displayGrayBuffer does not support darkMode parameter directly.
  // Dark mode grayscale rendering is handled at the pixel level in renderChar.
  display.displayGrayBuffer(turnOffScreen);
}

void GfxRenderer::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const {
  // Guard the uint16_t casts below: a negative would wrap to a huge length.
  assert(yStart >= 0 && numRows > 0 && yStart <= static_cast<int>(panelHeight) - numRows);
  display.writeGrayscalePlaneStrip(lsbPlane, scratch, static_cast<uint16_t>(yStart), static_cast<uint16_t>(numRows));
}

bool GfxRenderer::supportsStripGrayscale() const { return display.supportsStripGrayscale(); }

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing 48KB of contiguous memory.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  // Allocate and copy each chunk
  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(chunkSize));

    if (!bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! Failed to allocate BW buffer chunk %zu (%zu bytes)", i, chunkSize);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, chunkSize);
  }

  LOG_DBG("GFX", "Stored BW buffer in %zu chunks (%zu bytes each)", bwBufferChunks.size(), BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer() {
  // Check if all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    return;
  }

  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    memcpy(frameBuffer + offset, bwBufferChunks[i], chunkSize);
  }

  display.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  LOG_DBG("GFX", "Restored and freed BW buffer chunks");
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }
}

bool GfxRenderer::renderCharReader(const uint32_t cp, int* x, const int* y, const bool pixelState,
                                   const bool isCjk) const {
  FontManager& fm = FontManager::getInstance();
  if (!fm.isExternalFontEnabled()) return false;
  ExternalFont* extFont = fm.getActiveFont();
  if (!extFont) return false;

  const uint8_t* bitmap = extFont->getGlyph(cp);
  if (bitmap) {
    ExternalGlyphMetrics metrics = getDefaultMetrics(*extFont, cp);
    if (shouldUseCjkSymbolCellMetrics(cp)) {
      normalizeCjkSymbolMetricsForRendering(metrics, extFont->getCharWidth(), extFont->isRichMetricsFormat());
    }
    const int spacing = isCjk ? cjkSpacing : getAsciiSpacing(cp);
    const int advance = getExternalGlyphAdvanceForRendering(metrics, extFont->getCharWidth(), spacing, isCjk,
                                                            shouldUseGlyphBoundsForAdvance(cp));
    const int cellClipWidth = isCjk ? extFont->getCharWidth() : -1;
    renderExternalGlyph(bitmap, extFont, x, *y, pixelState, metrics, advance, cellClipWidth);
    return true;
  }
  // Missing glyph in external reader font - try built-in CJK UI font for CJK
  if (isCjk && CjkUiFont20::hasCjkUiGlyph(cp)) {
    renderBuiltinCjkGlyph(cp, x, *y, pixelState);
    return true;
  }
  return false;  // fall through to built-in reader font
}

bool GfxRenderer::renderCharUiCjk(const uint32_t cp, int* x, const int* y, const bool pixelState) const {
  FontManager& fm = FontManager::getInstance();
  const bool useExtUiFirst = fm.isUiFontEnabled();

  // When external UI font is enabled, try it first
  if (useExtUiFirst) {
    ExternalFont* uiExtFont = fm.getActiveUiFont();
    if (uiExtFont) {
      const uint8_t* bitmap = uiExtFont->getGlyph(cp);
      if (bitmap) {
        ExternalGlyphMetrics metrics = getDefaultMetrics(*uiExtFont, cp);
        int advanceCJK = 0;
        if (shouldUseCjkSymbolCellMetrics(cp)) {
          normalizeCjkSymbolMetricsForRendering(metrics, uiExtFont->getCharWidth(), uiExtFont->isRichMetricsFormat());
          advanceCJK = getExternalGlyphAdvanceForRendering(metrics, uiExtFont->getCharWidth(), 0, true, false);
        } else {
          advanceCJK = adjustNonRichAdvance(metrics, *uiExtFont);
        }
        renderExternalGlyph(bitmap, uiExtFont, x, *y, pixelState, metrics, advanceCJK);
        return true;
      }
    }
  }

  // Built-in CJK UI font (Flash access is fast) - used when no external
  // UI font, or as fallback when external font doesn't have this glyph
  if (CjkUiFont20::hasCjkUiGlyph(cp)) {
    renderBuiltinCjkGlyph(cp, x, *y, pixelState);
    return true;
  }

  // If external UI font wasn't tried first (not enabled), try it now as fallback
  if (!useExtUiFirst && fm.isUiFontEnabled()) {
    ExternalFont* uiExtFont = fm.getActiveUiFont();
    if (uiExtFont) {
      const uint8_t* bitmap = uiExtFont->getGlyph(cp);
      if (bitmap) {
        ExternalGlyphMetrics metrics = getDefaultMetrics(*uiExtFont, cp);
        // Non-rich fonts need descent adjustment to align with EPD font
        // baseline; left bearing is folded into advance by the helper.
        int adjustedY = *y;
        if (!uiExtFont->isRichMetricsFormat()) {
          adjustedY += CJK_UI_FONT_DESCENT;
        }
        int adv = 0;
        if (shouldUseCjkSymbolCellMetrics(cp)) {
          normalizeCjkSymbolMetricsForRendering(metrics, uiExtFont->getCharWidth(), uiExtFont->isRichMetricsFormat());
          adv = getExternalGlyphAdvanceForRendering(metrics, uiExtFont->getCharWidth(), 0, true, false);
        } else {
          adv = adjustNonRichAdvance(metrics, *uiExtFont);
        }
        renderExternalGlyph(bitmap, uiExtFont, x, adjustedY, pixelState, metrics, adv);
        return true;
      }
    }
  }

  // Last resort: try reader font if neither UI font has this glyph
  if (fm.isExternalFontEnabled()) {
    ExternalFont* extFont = fm.getActiveFont();
    if (extFont) {
      const uint8_t* bitmap = extFont->getGlyph(cp);
      if (bitmap) {
        ExternalGlyphMetrics metrics = getDefaultMetrics(*extFont, cp);
        if (shouldUseCjkSymbolCellMetrics(cp)) {
          normalizeCjkSymbolMetricsForRendering(metrics, extFont->getCharWidth(), extFont->isRichMetricsFormat());
        }
        // Non-rich fonts need descent adjustment to align with EPD font baseline
        int adjustedY = *y;
        if (!extFont->isRichMetricsFormat()) {
          adjustedY += CJK_UI_FONT_DESCENT;
        }
        renderExternalGlyph(bitmap, extFont, x, adjustedY, pixelState, metrics, metrics.advanceX);
        return true;
      }
    }
  }
  return false;
}

bool GfxRenderer::renderCharUiNonCjk(const uint32_t cp, int* x, const int* y, const bool pixelState,
                                     const bool allowBuiltInUiGlyph) const {
  FontManager& fm = FontManager::getInstance();
  const bool useExtUiFirst = fm.isUiFontEnabled();

  // Try external UI font first when enabled
  if (useExtUiFirst) {
    ExternalFont* uiExtFont = fm.getActiveUiFont();
    if (uiExtFont) {
      const uint8_t* bitmap = uiExtFont->getGlyph(cp);
      if (bitmap) {
        ExternalGlyphMetrics metrics = getDefaultMetrics(*uiExtFont, cp);
        int advanceNonCJK = 0;
        if (shouldUseCjkSymbolCellMetrics(cp)) {
          normalizeCjkSymbolMetricsForRendering(metrics, uiExtFont->getCharWidth(), uiExtFont->isRichMetricsFormat());
          advanceNonCJK = getExternalGlyphAdvanceForRendering(metrics, uiExtFont->getCharWidth(), 0, true, false);
        } else {
          advanceNonCJK = adjustNonRichAdvance(metrics, *uiExtFont);
        }
        renderExternalGlyph(bitmap, uiExtFont, x, *y, pixelState, metrics, advanceNonCJK);
        return true;
      }
    }
  }

  // Built-in CJK UI font (covers some non-CJK punctuation/symbols too)
  if (allowBuiltInUiGlyph && CjkUiFont20::hasCjkUiGlyph(cp)) {
    renderBuiltinCjkGlyph(cp, x, *y, pixelState);
    return true;
  }

  // If external UI font wasn't tried first, try it now as fallback
  if (!useExtUiFirst && fm.isUiFontEnabled()) {
    ExternalFont* uiExtFont = fm.getActiveUiFont();
    if (uiExtFont) {
      const uint8_t* bitmap = uiExtFont->getGlyph(cp);
      if (bitmap) {
        ExternalGlyphMetrics metrics = getDefaultMetrics(*uiExtFont, cp);
        // Non-rich fonts need descent adjustment; left bearing is folded
        // into advance by the helper.
        int adjustedY = *y;
        if (!uiExtFont->isRichMetricsFormat()) {
          adjustedY += CJK_UI_FONT_DESCENT;
        }
        int adv = 0;
        if (shouldUseCjkSymbolCellMetrics(cp)) {
          normalizeCjkSymbolMetricsForRendering(metrics, uiExtFont->getCharWidth(), uiExtFont->isRichMetricsFormat());
          adv = getExternalGlyphAdvanceForRendering(metrics, uiExtFont->getCharWidth(), 0, true, false);
        } else {
          adv = adjustNonRichAdvance(metrics, *uiExtFont);
        }
        renderExternalGlyph(bitmap, uiExtFont, x, adjustedY, pixelState, metrics, adv);
        return true;
      }
    }
  }
  return false;  // fall through to EPD font rendering
}

void GfxRenderer::renderChar(const int fontId, const EpdFontFamily& fontFamily, const uint32_t cp, int* x, const int* y,
                             const bool pixelState, const EpdFontFamily::Style style) const {
  // Cache character classification result to avoid repeated calls (perf opt)
  const bool isCjk = isCjkCodepoint(cp);

  // Try external/UI font strategies; fall through to EPD font on miss.
  if (isReaderFont(fontId)) {
    if (renderCharReader(cp, x, y, pixelState, isCjk)) return;
  } else if (isCjk) {
    if (renderCharUiCjk(cp, x, y, pixelState)) return;
  } else {
    const bool allowBuiltInUiGlyph = fontFamily.getGlyph(cp, style) == nullptr;
    if (renderCharUiNonCjk(cp, x, y, pixelState, allowBuiltInUiGlyph)) return;
  }

  // EPD font fallback
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    glyph = fontFamily.getGlyph('?', style);
  }

  // no glyph? Still advance cursor to prevent overlap
  if (!glyph) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    if (isCjk) {
      *x += 20;  // Full-width character default
    } else {
      *x += 10;  // Half-width character default
    }
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;

  const uint8_t* bitmap = getGlyphBitmap(fontData, glyph);

  if (bitmap != nullptr) {
    for (int glyphY = 0; glyphY < height; glyphY++) {
      const int screenY = *y - glyph->top + glyphY;
      for (int glyphX = 0; glyphX < width; glyphX++) {
        const int pixelPosition = glyphY * width + glyphX;
        const int screenX = *x + left + glyphX;

        if (is2Bit) {
          const uint8_t byte = bitmap[pixelPosition / 4];
          const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
          const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

          if (renderMode == BW) {
            if (bmpVal < 3) {
              drawPixel(screenX, screenY, pixelState);
            }
          } else if (renderMode == GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
            drawPixel(screenX, screenY, false);
          } else if (renderMode == GRAYSCALE_LSB && bmpVal == 1) {
            drawPixel(screenX, screenY, false);
          }
        } else {
          const uint8_t byte = bitmap[pixelPosition / 8];
          const uint8_t bit_index = 7 - (pixelPosition % 8);

          if ((byte >> bit_index) & 1) {
            drawPixel(screenX, screenY, pixelState);
          }
        }
      }
    }
  }

  *x += fp4::toPixel(glyph->advanceX);
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}

int GfxRenderer::getAsciiSpacing(const uint32_t cp) const {
  if (isAsciiDigit(cp)) return asciiDigitSpacing;
  if (isAsciiLetter(cp)) return asciiLetterSpacing;
  return 0;
}

// Check if fontId is a reader font (should use external Chinese font)
// UI fonts (UI_10, UI_12, SMALL_FONT) should NOT use external font
bool GfxRenderer::isReaderFont(const int fontId) {
  // First check if it's a UI font - UI fonts should NOT use external reader
  // font
  for (int i = 0; i < UI_FONT_COUNT; i++) {
    if (UI_FONT_IDS[i] == fontId) {
      return false;  // This is a UI font, not a reader font
    }
  }

  // External font IDs are negative (from CrossPointSettings::getReaderFontId())
  if (fontId < 0) {
    return true;
  }

  for (int i = 0; i < READER_FONT_COUNT; i++) {
    if (READER_FONT_IDS[i] == fontId) {
      return true;
    }
  }
  return false;
}

// Get effective font ID, handling fallback for external reader font IDs
// When external font is selected (negative ID) but not available, fall back to
// built-in font
int GfxRenderer::getEffectiveFontId(int fontId) const {
  // Only negative IDs that are reader fonts need fallback
  // UI fonts have negative IDs too but should not be redirected
  if (fontId < 0 && isReaderFont(fontId)) {
    // This is an external reader font ID, use the selected built-in reader
    // font as fallback
    return readerFallbackFontId != 0 ? readerFallbackFontId : READER_FONT_IDS[0];
  }
  return fontId;
}

void GfxRenderer::renderExternalGlyph(const uint8_t* bitmap, ExternalFont* font, int* x, const int lineTopY,
                                      const bool pixelState, const ExternalGlyphMetrics& metrics,
                                      const int advanceOverride, const int cellClipWidth) const {
  const uint8_t width = metrics.width > 0 ? metrics.width : font->getCharWidth();
  const uint8_t height = metrics.height > 0 ? metrics.height : font->getCharHeight();
  const uint8_t bytesPerRow = (width + 7) / 8;
  const ExternalGlyphLayout layout = computeExternalGlyphLayout(*x, lineTopY, *font, metrics, advanceOverride);
  const int cursorX = *x;
  const int screenWidth = getScreenWidth();
  const int screenHeight = getScreenHeight();
  int minGlyphX = std::max(0, -layout.drawX);
  int maxGlyphX = std::min<int>(width, screenWidth - layout.drawX);
  const int minGlyphY = std::max(0, -layout.drawY);
  const int maxGlyphY = std::min<int>(height, screenHeight - layout.drawY);

  if (cellClipWidth > 0) {
    minGlyphX = std::max(minGlyphX, cursorX - layout.drawX);
    maxGlyphX = std::min(maxGlyphX, cursorX + cellClipWidth - layout.drawX);
  }

  if (minGlyphX >= maxGlyphX || minGlyphY >= maxGlyphY) {
    *x += layout.advanceX;
    return;
  }

  for (int glyphY = minGlyphY; glyphY < maxGlyphY; glyphY++) {
    const int screenY = layout.drawY + glyphY;
    for (int glyphX = minGlyphX; glyphX < maxGlyphX; glyphX++) {
      const int byteIndex = glyphY * bytesPerRow + (glyphX / 8);
      const int bitIndex = 7 - (glyphX % 8);  // MSB first

      if ((bitmap[byteIndex] >> bitIndex) & 1) {
        drawPixel(layout.drawX + glyphX, screenY, pixelState);
      }
    }
  }

  *x += layout.advanceX;
}

void GfxRenderer::renderBuiltinCjkGlyph(const uint32_t cp, int* x, const int y, const bool pixelState) const {
  // Use built-in 20px CJK UI font
  const uint8_t* bitmap = CjkUiFont20::getCjkUiGlyph(cp);
  const uint8_t fontWidth = CjkUiFont20::CJK_UI_FONT_WIDTH;
  const uint8_t fontHeight = CjkUiFont20::CJK_UI_FONT_HEIGHT;
  const uint8_t bytesPerRow = CjkUiFont20::CJK_UI_FONT_BYTES_PER_ROW;
  const uint8_t bytesPerChar = CjkUiFont20::CJK_UI_FONT_BYTES_PER_CHAR;
  const uint8_t actualWidth = CjkUiFont20::getCjkUiGlyphWidth(cp);

  if (!bitmap || actualWidth == 0) {
    return;
  }

  // Check if glyph is empty (all zeros) - skip rendering but still advance
  // cursor
  bool hasContent = false;
  for (int i = 0; i < bytesPerChar; i++) {
    if (pgm_read_byte(&bitmap[i]) != 0) {
      hasContent = true;
      break;
    }
  }

  if (hasContent) {
    const int startY = y - fontHeight + 4;  // 4px descent for CJK characters

    for (int glyphY = 0; glyphY < fontHeight; glyphY++) {
      const int screenY = startY + glyphY;
      for (int glyphX = 0; glyphX < fontWidth; glyphX++) {
        const int byteIndex = glyphY * bytesPerRow + (glyphX / 8);
        const int bitIndex = 7 - (glyphX % 8);  // MSB first

        // Read from PROGMEM
        const uint8_t byte = pgm_read_byte(&bitmap[byteIndex]);
        if ((byte >> bitIndex) & 1) {
          drawPixel(*x + glyphX, screenY, pixelState);
        }
      }
    }
  }

  // Advance cursor by actual width (proportional spacing)
  *x += actualWidth;
}
