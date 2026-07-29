#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

#include "ExternalFontCachePolicy.h"

struct ExternalGlyphMetrics {
  uint8_t width = 0;
  uint8_t height = 0;
  uint16_t advanceX = 0;
  uint16_t flags = 0;
  int16_t left = 0;
  int16_t top = 0;
};

struct ExternalFontMetrics {
  int16_t ascender = 0;
  int16_t descender = 0;
  uint16_t lineHeight = 0;
};

// One Unicode interval entry from an EPDFont file. The interval table maps
// codepoint ranges to a contiguous slice of the per-glyph metadata table:
// for a codepoint cp in [start, end], its glyph index is
//   glyphOffset + (cp - start)
struct EpdInterval {
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t glyphOffset = 0;  // cumulative glyph count from earlier intervals
};

/**
 * External font loader. Two on-disk formats are supported:
 *
 *   1. Legacy Xteink ".bin": fixed-grid, direct codepoint indexing.
 *      Filename: FontName_size_WxH.bin
 *      Layout:   raw glyph bitmap[codepoint], each glyph = bytesPerRow * H,
 *                row-aligned MSB-first 1-bit packing.
 *
 *   2. EPDFont ".epdf" (rich-metrics, produced by the x4-epdfont-converter):
 *      Filename: FontName_size_WxH.epdf  (W and H are nominal cell size used
 *                                          by the renderer for CJK layout)
 *      Layout:   32-byte header + interval table + per-glyph metrics table
 *                + variable-size bitmap blob, sequentially-packed 1-bit MSB
 *                first (or 2-bit, but 2-bit fonts are rejected at load time
 *                because the renderer only supports 1-bit external glyphs).
 *      Lookup:   binary-search the interval table for codepoint, derive
 *                glyph index, read 16-byte glyph entry, then read the bitmap
 *                slice.
 *      The loader converts each glyph bitmap from sequential packing to
 *      row-aligned packing on cache insertion so the renderer can keep using
 *      the same drawing path it uses for legacy ".bin" glyphs.
 */
class ExternalFont {
 public:
  ExternalFont() = default;
  ~ExternalFont();

  // Disable copy
  ExternalFont(const ExternalFont&) = delete;
  ExternalFont& operator=(const ExternalFont&) = delete;

  /**
   * Load font from .bin or .epdf file.
   * @param filepath Full path on SD card (e.g.
   * "/fonts/KingHwaOldSong_38_33x39.bin")
   * @return true on success
   */
  bool load(const char* filepath);

  /**
   * Get glyph bitmap data (with LRU cache)
   * @param codepoint Unicode codepoint
   * @return Bitmap data pointer, nullptr if char not found
   */
  const uint8_t* getGlyph(uint32_t codepoint);

  /**
   * Preload multiple glyphs at once (optimized for batch SD reads)
   * Call this before rendering a chapter to
   * warm up the cache
   * @param codepoints Array of unicode codepoints to preload
   * @param count Number of
   * codepoints in the array
   * @param isCancelled Optional callback checked between glyph loads
   * @param
   * cancellationContext Context supplied to isCancelled
   * @return false when cancelled, true otherwise
   */
  bool preloadGlyphs(const uint32_t* codepoints, size_t count, bool (*isCancelled)(const void*) = nullptr,
                     const void* cancellationContext = nullptr);

  // Font properties
  uint8_t getCharWidth() const { return _charWidth; }
  uint8_t getCharHeight() const { return _charHeight; }
  uint8_t getBytesPerRow() const { return _bytesPerRow; }
  uint16_t getBytesPerChar() const { return _bytesPerChar; }
  const char* getFontName() const { return _fontName; }
  uint8_t getFontSize() const { return _fontSize; }
  size_t getCacheCapacity() const { return CACHE_SIZE; }
  size_t getPreloadLimit() const { return PRELOAD_LIMIT; }

  bool isLoaded() const { return _isLoaded; }
  bool isRichMetricsFormat() const { return _isRichMetricsFormat; }
  int16_t getAscender() const { return _fontMetrics.ascender; }
  int16_t getDescender() const { return _fontMetrics.descender; }
  uint16_t getLineHeight() const { return _fontMetrics.lineHeight; }
  void unload();

  // Release bitmap glyph cache while keeping font metadata/file state loaded.
  // Layout measurement uses getGlyphMetricsForLayout() and can run without this cache.
  void releaseGlyphCache();

  /**
   * Check if a font with given dimensions can fit in the glyph cache.
   * Fonts with bytesPerChar > MAX_GLYPH_BYTES cannot be loaded.
   */
  static bool canFitGlyph(uint8_t width, uint8_t height) {
    return static_cast<uint16_t>(((width + 7) / 8)) * height <= MAX_GLYPH_BYTES;
  }

  /**
   * Get glyph metrics using cached data when available, otherwise the layout
   * metrics path. Does not require callers to load the bitmap glyph first.
   * @param cp Unicode codepoint
   * @param outMinX Minimum X offset (left bearing)
   * @param outAdvanceX Advance width for cursor positioning
   * @return true if metrics are available, false otherwise
   */
  bool getGlyphMetrics(uint32_t cp, uint8_t* outMinX, uint8_t* outAdvanceX);
  bool getGlyphMetrics(uint32_t codepoint, ExternalGlyphMetrics* out) const;

  /**
   * Measure a glyph for layout without requiring the full bitmap glyph cache.
   * Mirrors getGlyph() fallback semantics so section builds keep identical line breaks.
   */
  bool getGlyphMetricsForLayout(uint32_t codepoint, ExternalGlyphMetrics* out) const;

 private:
  // Font file handle (keep open to avoid repeated open/close)
  mutable HalFile _fontFile;
  bool _isLoaded = false;

  // Properties parsed from filename
  char _fontName[32] = {0};
  uint8_t _fontSize = 0;
  uint8_t _charWidth = 0;
  uint8_t _charHeight = 0;
  uint8_t _bytesPerRow = 0;
  uint16_t _bytesPerChar = 0;
  bool _isRichMetricsFormat = false;

  // EPDFont format metadata (only valid when _isRichMetricsFormat is true)
  ExternalFontMetrics _fontMetrics;
  EpdInterval* _intervals = nullptr;
  uint32_t _intervalCount = 0;
  uint32_t _glyphCount = 0;
  uint32_t _glyphsOffset = 0;  // file offset of the glyph metrics table
  uint32_t _bitmapOffset = 0;  // file offset of the bitmap blob

  // LRU cache - lazily allocated for bitmap rendering/preload, freed on unload()
  // or before memory-heavy section builds. 160 glyphs for CJK text rendering
  // (~44KB per font when loaded).
  static constexpr int CACHE_SIZE = ExternalFontCachePolicy::kGlyphCacheSize;
  static constexpr int PRELOAD_LIMIT = ExternalFontCachePolicy::kPreloadLimit;
  static constexpr int MAX_GLYPH_BYTES = 260;  // Max 260 bytes per glyph (e.g. up to 38x52)

  struct CacheEntry {
    uint32_t codepoint = 0xFFFFFFFF;  // Invalid marker
    uint8_t bitmap[MAX_GLYPH_BYTES];
    uint32_t lastUsed = 0;
    bool notFound = false;              // True if glyph doesn't exist in font
    ExternalGlyphMetrics metrics = {};  // Cached rendering metrics
  };
  CacheEntry* _cache = nullptr;  // Lazily allocated when bitmap rendering/preload needs it
  uint32_t _accessCounter = 0;

  // Scratch bitmap for legacy .bin layout metrics. This avoids a 260-byte stack
  // object and repeated heap allocation while keeping full glyph cache suspended.
  mutable uint8_t _metricsScratch[MAX_GLYPH_BYTES] = {};

  // Sequential read fast path - stores the absolute file offset expected for
  // the next read, so adjacent glyph records/bitmaps can skip seek().
  mutable uint32_t _lastReadOffset = 0;
  mutable bool _hasLastReadOffset = false;

  // Simple hash table for O(1) cache lookup (codepoint -> cache index, -1 if
  // not cached)
  int16_t* _hashTable = nullptr;  // Dynamically allocated on load()
  static int hashCodepoint(uint32_t cp) { return cp % CACHE_SIZE; }

  /**
   * Parse and validate the EPDFont 32-byte header. The file pointer is
   * left positioned at the start of the interval table on success.
   */
  bool loadEpdFontHeader();

  /**
   * Read the EPDFont interval table into _intervals.
   * Caller must have already populated _intervalCount and seek'd to
   * the intervals offset.
   */
  bool readEpdIntervals();

  /**
   * Look up the interval index containing `codepoint`. Returns -1 if the
   * codepoint is outside every interval. Uses binary search since
   * intervals are sorted by start.
   */
  int findEpdInterval(uint32_t codepoint) const;

  /**
   * Read a 16-byte glyph entry by index from the on-disk metrics table.
   * Populates `out` (width/height/advanceX/left/top, dataLength,
   * dataOffset). Returns false on I/O error.
   */
  bool readEpdGlyphEntry(uint32_t glyphIndex, ExternalGlyphMetrics* out, uint32_t* outDataLength,
                         uint32_t* outDataOffset) const;

  /**
   * Read variable-length glyph bitmap from SD into a temporary buffer,
   * then transcode it from EPDFont's sequential 1-bit packing into the
   * row-aligned packing the renderer expects. Output goes into `dst`
   * (must be at least bytesPerRow(width) * height bytes wide).
   */
  bool readEpdGlyphBitmap(uint32_t dataOffset, uint32_t dataLength, uint8_t width, uint8_t height, uint8_t* dst) const;

  /**
   * Read legacy ".bin" glyph bitmap (fixed-grid direct codepoint indexing).
   */
  bool readLegacyGlyphFromSD(uint32_t codepoint, uint8_t* buffer) const;

  bool ensureGlyphCache();
  bool measureRichGlyphForLayout(uint32_t codepoint, ExternalGlyphMetrics* out) const;
  bool measureLegacyGlyphForLayout(uint32_t codepoint, ExternalGlyphMetrics* out) const;

  /**
   * Parse filename to get font parameters
   * Format: FontName_size_WxH.{bin,epdf}
   */
  bool parseFilename(const char* filename);

  /**
   * Find glyph in cache
   * @return Cache index, -1 if not found
   */
  int findInCache(uint32_t codepoint) const;

  /**
   * Get LRU cache slot (least recently used)
   */
  int getLruSlot();
};
