#pragma once

class HalFile;

#include <cstdint>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"

// On-disk binary format version for .cpfont files. Defined as a preprocessor
// macro (rather than a constexpr) so it can be stringified into the SD-fonts
// release URL — see FONT_MANIFEST_URL in FontDownloadActivity.h. No integer
// suffix because stringification would include it (e.g. `4U` → `"4U"`).
//
// The canonical version for the build tooling lives in
// lib/EpdFont/scripts/cpfont_version.py. This firmware-side copy must be
// bumped manually when the firmware is updated to support a new format.
// Reader enforcement: SdCardFont::load().
#define CPFONT_VERSION 4

class SdCardFont {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_STYLES = 4;
  using CancellationCallback = bool (*)(const void*);
  static constexpr int PREWARM_FAILED = -1;
  static constexpr int PREWARM_CANCELLED = -2;

  SdCardFont() = default;
  ~SdCardFont();
  // Owns raw buffers freed in dtor — no shallow-copy semantics. Make any
  // accidental pass-by-value or move a compile-time error.
  SdCardFont(const SdCardFont&) = delete;
  SdCardFont& operator=(const SdCardFont&) = delete;
  SdCardFont(SdCardFont&&) = delete;
  SdCardFont& operator=(SdCardFont&&) = delete;

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  bool load(const char* path);

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // styleMask: bitmask of styles to prewarm (bit 0=regular, 1=bold, 2=italic, 3=bolditalic).
  // Default 0x0F = all present styles.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data).
  // Returns number of glyphs that couldn't be loaded (0 on full success), or
  // PREWARM_FAILED on allocation/IO failure, or PREWARM_CANCELLED when the
  // optional callback aborts the operation.
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false,
              CancellationCallback isCancelled = nullptr, const void* cancellationContext = nullptr);

  // Build a compact advance-only table for layout measurement.
  // Extracts ALL unique codepoints from words (no MAX_PAGE_GLYPHS cap),
  // batch-reads advanceX from SD, stores in a sorted per-style table.
  // extraText: optional additional codepoints to warm in the same SD pass
  // (e.g. shaped Arabic presentation forms the measurement path will look up).
  // Returns number of codepoints not found in font coverage.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F, const char* extraText = nullptr);
  int buildAdvanceTable(const std::vector<std::string>& words, bool includeHyphen, uint8_t styleMask = 0x0F,
                        const char* extraText = nullptr);

  // Look up advanceX for a codepoint from the advance table.
  // Returns the 12.4 fixed-point advance, or 0 if not found.
  uint16_t getAdvance(uint32_t codepoint, uint8_t style) const;

  // Read glyph metadata without loading or allocating its bitmap. Used by UI
  // measurement so width calculations do not churn the on-demand bitmap ring.
  bool getGlyphMetrics(uint32_t codepoint, uint8_t style, EpdGlyph* outGlyph, HalFile* metricsFile = nullptr);
  // Reuse a caller-owned file only for one drawText scope. SdCardFont never owns
  // or closes this handle; endOverflowRead() must run before the caller destroys it.
  bool beginOverflowRead(HalFile& file);
  void endOverflowRead(HalFile& file);
  // Returns true if advance table is populated for at least one style.
  bool hasAdvanceTable() const;

  // Free mini data for all styles and restore stub EpdFontData.
  // Preserves the persistent advance cache so repeated layout passes can reuse
  // previously fetched metrics.
  void clearCache();

  // Drop the persistent advance cache. Call when unloading the SD font or
  // when font/size/family/glyph-table state changes.
  void clearPersistentCache();

  // Returns pointer to the managed EpdFont for a given style.
  // Returns nullptr if the style is not present.
  EpdFont* getEpdFont(uint8_t style = 0);

  // Returns true if the given style is present in this font file.
  bool hasStyle(uint8_t style) const;

  // Resolve requested style bits to the closest present style.
  uint8_t resolveStyle(uint8_t style) const;

  // Resolve every requested style bit through fallback and return the actual
  // styles that need cache/advance preparation.
  uint8_t resolveStyleMask(uint8_t styleMask) const;

  // Number of styles present in this font file.
  uint8_t styleCount() const { return styleCount_; }

  // Returns true if the glyph pointer points into the overflow buffer.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;

  // Returns the bitmap for an on-demand-loaded (overflow) glyph.
  const uint8_t* getOverflowBitmap(const EpdGlyph* glyph) const;

  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);

  struct Stats {
    uint32_t prewarmTotalMs = 0;
    uint32_t sdReadTimeMs = 0;
    uint32_t seekCount = 0;
    uint32_t uniqueGlyphs = 0;
    uint32_t bitmapBytes = 0;
  };
  void logStats(const char* label = "SDCF");
  void resetStats();
  const Stats& getStats() const { return stats_; }

  // Content hash of the file header + style TOC entries (computed during load).
  // Used to generate deterministic font IDs for section cache invalidation.
  uint32_t contentHash() const { return contentHash_; }

 private:
  // Per-style metadata (parsed from file header/TOC)
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  // All per-style data: file offsets, intervals, kern/lig, prewarm cache, EpdFont
  struct PerStyle {
    CpFontHeader header{};

    // File layout offsets for this style's data sections
    uint32_t intervalsFileOffset = 0;
    uint32_t glyphsFileOffset = 0;
    uint32_t kernLeftFileOffset = 0;
    uint32_t kernRightFileOffset = 0;
    uint32_t kernMatrixFileOffset = 0;
    uint32_t ligatureFileOffset = 0;
    uint32_t bitmapFileOffset = 0;

    // Full intervals loaded from file (kept in RAM for codepoint lookup).
    // Sparse BMP fonts can instead use a page-packed coverage bitmap: interval
    // tables for some CJK fonts exceed the largest free heap block after a
    // network download, while the bitmap stores only pages containing glyphs.
    EpdUnicodeInterval* fullIntervals = nullptr;
    EPD_PACKED_BEGIN
    struct BmpInterval16 {
      uint16_t first;
      uint16_t last;
      uint16_t offset;
    } EPD_PACKED_ATTR;
    EPD_PACKED_END
    static_assert(sizeof(BmpInterval16) == 6, "BmpInterval16 must remain compact");
    BmpInterval16* bmpIntervals = nullptr;
    bool intervalsAreBmp16 = false;
    static constexpr uint16_t BMP_PAGE_COUNT = 256;
    static constexpr uint16_t BMP_PAGE_MAP_WORDS = BMP_PAGE_COUNT / 32;
    static constexpr uint16_t BMP_WORDS_PER_PAGE = 8;
    uint32_t bmpCoveredPages[BMP_PAGE_MAP_WORDS] = {};
    uint32_t* bmpCoverageData = nullptr;
    uint16_t bmpCoveragePageCount = 0;
    uint16_t bmpMiniBitmapCapacity = 0;
    bool intervalsAreBmpCoverage = false;

    // Persistent kern-class + ligature tables (lazy-loaded on first prewarm).
    // The full kern MATRIX is NOT resident — on Literata-class fonts a single
    // style's matrix is ~36-42KB contiguous, and 4 styles' worth won't fit
    // alongside bitmaps + framebuffer on a 380KB device. Only kernLeftClasses
    // and kernRightClasses (small codepoint→classId tables, ~3KB each) stay
    // resident; the matrix is reconstructed per-page as miniKernMatrix.
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    bool kernLigLoaded = false;

    // Stub EpdFontData returned when not prewarmed
    EpdFontData stubData{};

    // Mini EpdFontData built during prewarm. Buffers are kept-if-fits across pages
    // (capacities below track allocated sizes): freeing and reallocating slightly
    // different sizes on every page turn was a primary heap fragmenter — each page's
    // freed hole rarely fit the next page's need, so maxAlloc eroded all session.
    // The per-render PrewarmScope calls clearCache() -> resetStyleMiniData(), which
    // keeps both the allocations AND the loaded data. Buffers: reuse means
    // ensureArrayCapacity early-returns once capacities converge on the book's
    // max, so page turns stop touching the allocator (the free/realloc-per-page
    // pattern was a primary heap fragmenter). Data: the next prewarm
    // subset-checks against the resident tables (see prewarmStyle), so the idle
    // prewarm of page N+1 serves the actual turn with zero SD reads. Retention
    // is bounded two ways in resetStyleMiniData(): a heap floor frees outright
    // under pressure, and sustained underuse (an outlier page's oversized bitmap
    // arena) frees after a few consecutive low-use rebuilds. freeStyleMiniData()
    // remains the full teardown (zeroes capacities) for style eviction / font
    // unload.
    EpdFontData miniData{};
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;
    uint32_t miniIntervalCapacity = 0;
    uint32_t miniGlyphCapacity = 0;
    uint32_t miniBitmapCapacity = 0;
    bool miniBitmapUsesCoverageArena = false;
    // Bitmap bytes the current page actually used (set by prewarmStyle), the
    // underuse-hysteresis signal; 0 = no bitmap built this scope (metadata-only
    // prewarm), which leaves the hysteresis counter untouched.
    uint32_t miniBitmapUsed = 0;
    uint8_t miniUnderuseRuns = 0;
    // True when the resident mini was built metadata-only (no bitmaps): it can
    // serve metadata requests but a full render request must rebuild.
    bool miniMetadataOnly = false;
    // Set by a rebuild, consumed by resetStyleMiniData: gates the underuse
    // hysteresis to one evaluation per rebuild (scopes reset twice, and subset
    // hits load nothing new to judge).
    bool miniHysteresisPending = false;

    // Per-page mini kern matrix (built by buildMiniKernMatrix on each full
    // prewarm). miniKernLeftClasses/miniKernRightClasses map ONLY the codepoints
    // used on the current page to renumbered class IDs (1..miniKern*ClassCount).
    // miniKernMatrix is a small miniKernLeftClassCount × miniKernRightClassCount
    // flat matrix. Typical Latin page: ~25×25 matrix = ~625 bytes per style vs
    // ~36KB for the full Literata matrix — ~50× reduction.
    EpdKernClassEntry* miniKernLeftClasses = nullptr;
    EpdKernClassEntry* miniKernRightClasses = nullptr;
    uint16_t miniKernLeftEntryCount = 0;
    uint16_t miniKernRightEntryCount = 0;
    uint8_t miniKernLeftClassCount = 0;
    uint8_t miniKernRightClassCount = 0;
    int8_t* miniKernMatrix = nullptr;
    // Kept-if-fits capacities, same rationale as the mini glyph buffers above.
    uint16_t miniKernLeftCapacity = 0;
    uint16_t miniKernRightCapacity = 0;
    uint32_t miniKernMatrixCapacity = 0;

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // Shared on-demand overflow cache. A UI page commonly contains dozens of
  // distinct glyphs; eight entries caused the entire page to reload from SD on
  // each measure/draw traversal. Keep enough slots for a normal page while also
  // enforcing a bitmap-byte budget so unusual large glyphs cannot consume the
  // TLS/reader heap reserve.
  static constexpr uint32_t OVERFLOW_CAPACITY = 80;
  static constexpr uint32_t OVERFLOW_BITMAP_BUDGET_BYTES = 24 * 1024;
  static constexpr uint32_t OVERFLOW_LOW_HEAP_RESERVE_BYTES = 8 * 1024;
  uint32_t overflowBitmapBudget() const;
  struct OverflowEntry {
    EpdGlyph glyph;
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
  };
  OverflowEntry overflow_[OVERFLOW_CAPACITY] = {};
  uint32_t overflowCount_ = 0;
  uint32_t overflowNext_ = 0;
  uint32_t overflowOldest_ = 0;
  uint32_t overflowBitmapBytes_ = 0;
  // Tiny fixed metric cache for repeated UI measure/draw layout calls. Unlike
  // overflow_, these entries never own bitmap data and therefore stay cheap.
  static constexpr uint8_t METRICS_CACHE_CAPACITY = 32;
  struct MetricsCacheEntry {
    EpdGlyph glyph{};
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
    bool valid = false;
  };
  MetricsCacheEntry metricsCache_[METRICS_CACHE_CAPACITY] = {};
  uint8_t metricsCacheNext_ = 0;
  // Compact advance-only table for layout measurement (per-style).
  // Non-owning pointer valid only between beginOverflowRead()/endOverflowRead().
  HalFile* overflowFile_ = nullptr;
  // Built by buildAdvanceTable(), queried by getAdvance().
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
  };
  // Per-style advance table. Sorted by codepoint for binary lookup.
  // Bounded to ADVANCE_CACHE_LIMIT entries; persists across layout passes
  // (across calls to clearCache()) so repeated indexing of the same font
  // amortizes SD reads. Cleared only on font unload or clearPersistentCache().
  static constexpr uint32_t ADVANCE_CACHE_LIMIT = 768;
  AdvanceEntry* advanceTable_[MAX_STYLES] = {};
  uint32_t advanceTableSize_[MAX_STYLES] = {};
  bool advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const;
  // Merge sortedNew (sorted by codepoint, no overlap with existing) into the
  // advance table for styleIdx, preserving sort order; cap-truncates the tail.
  void mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount);

  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  bool prewarmCancelled_ = false;
  CancellationCallback prewarmCancellationCallback_ = nullptr;
  const void* prewarmCancellationContext_ = nullptr;

  // Per-style helpers
  void freeStyleMiniData(PerStyle& s);
  // Per-scope variant: drop the page's data, keep the allocations (see the
  // PerStyle comment). May escalate to freeStyleMiniData under heap pressure
  // or sustained underuse.
  void resetStyleMiniData(PerStyle& s);
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  void freeStyleMiniKern(PerStyle& s);
  bool loadStyleKernLigatureData(PerStyle& s);
  bool buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount);
  void applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const;
  void applyGlyphMissCallback(uint8_t styleIdx);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  bool findCachedGlyphMetrics(uint32_t codepoint, uint8_t styleIdx, EpdGlyph* outGlyph) const;
  int fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask);
  template <typename Iter>
  int buildAdvanceTableRange(Iter begin, Iter end, bool includeSpace, bool includeHyphen, uint8_t styleMask,
                             const char* extraText = nullptr);
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly);
  static bool cancellationRequested(CancellationCallback isCancelled, const void* cancellationContext);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);

  // Static callback for EpdFontData::coverageHandler: answers hasCodepoint()
  // from the RAM-resident full interval table, without SD I/O.
  static bool onCoverageQuery(void* ctx, uint32_t codepoint);
};
