#pragma once

#include <cstdint>

#include "ExternalFont.h"

class HalFile;

/**
 * Font information structure
 */
struct FontInfo {
  char filename[64];  // Full filename
  char name[32];      // Font name
  uint8_t size;       // Font size (pt)
  uint8_t width;      // Character width
  uint8_t height;     // Character height
};

/**
 * Font Manager - Singleton pattern
 * Manages font scanning, selection, and settings persistence
 * Supports two font slots: Reader font (for book content) and UI font (for
 * menus/titles)
 */
class FontManager {
 public:
  static FontManager& getInstance();

  // Disable copy
  FontManager(const FontManager&) = delete;
  FontManager& operator=(const FontManager&) = delete;

  /**
   * Scan /fonts/ directory to get available font list
   */
  void scanFonts();

  /**
   * Get font count
   */
  int getFontCount() const { return _fontCount; }

  /**
   * Get font info
   * @param index Font index (0 to getFontCount()-1)
   */
  const FontInfo* getFontInfo(int index) const;

  /**
   * Select reader font (for book content)
   * @param index Font index, -1 means disable external font (use built-in)
   */
  void selectFont(int index);

  /**
   * Select UI font (for menus, titles, etc.)
   * @param index Font index, -1 means disable (fallback to reader font or
   * built-in)
   */
  void selectUiFont(int index);

  /**
   * Temporarily load reader font without saving settings.
   */
  bool previewFont(int index);

  /**
   * Temporarily load UI font without saving settings.
   */
  bool previewUiFont(int index);

  /**
   * Restore reader/UI selections without saving settings.
   */
  void restoreFontSelection(int readerIndex, int uiIndex);

  /**
   * Get currently selected reader font index
   * @return -1 means using built-in font
   */
  int getSelectedIndex() const { return _selectedIndex; }

  /**
   * Get currently selected UI font index
   * @return -1 means using reader font fallback
   */
  int getUiSelectedIndex() const { return _selectedUiIndex; }

  /**
   * Get currently active reader font
   * @return Font pointer, nullptr if not enabled
   */
  ExternalFont* getActiveFont();

  /**
   * Get currently active UI font
   * @return Font pointer, nullptr if not enabled (will fallback to reader font)
   */
  ExternalFont* getActiveUiFont();

  /**
   * Release bitmap glyph caches for active reader/UI fonts while preserving
   * selected font metadata and open font files.
   */
  void releaseGlyphCaches();

  void setGlyphCachesSuspended(bool suspended);
  bool areGlyphCachesSuspended() const { return _glyphCachesSuspended; }
  bool isGlyphCacheSuspendedFor(const ExternalFont* font) const;

  class ScopedGlyphCacheSuspension {
   public:
    explicit ScopedGlyphCacheSuspension(FontManager& manager);
    ~ScopedGlyphCacheSuspension();
    ScopedGlyphCacheSuspension(const ScopedGlyphCacheSuspension&) = delete;
    ScopedGlyphCacheSuspension& operator=(const ScopedGlyphCacheSuspension&) = delete;

   private:
    FontManager& _manager;
    bool _previous;
  };

  /**
   * Check if external reader font is enabled
   */
  bool isExternalFontEnabled() const { return _selectedIndex >= 0 && _activeFont.isLoaded(); }

  /**
   * Check if external UI font is enabled
   */
  bool isUiFontEnabled() const {
    if (_selectedUiIndex < 0) {
      return false;
    }
    if (isUiSharingReaderFont()) {
      return _activeFont.isLoaded();
    }
    return _activeUiFont.isLoaded();
  }

  /**
   * Save settings to SD card
   */
  void saveSettings();

  /**
   * Load settings from SD card
   */
  void loadSettings();

 private:
  FontManager() {
    // Initialize font array
    for (int i = 0; i < MAX_FONTS; i++) {
      _fonts[i].filename[0] = '\0';
      _fonts[i].name[0] = '\0';
      _fonts[i].size = 0;
      _fonts[i].width = 0;
      _fonts[i].height = 0;
    }
  }

  static constexpr int MAX_FONTS = 16;
  static constexpr const char* FONTS_DIR = "/fonts";
  static constexpr const char* SETTINGS_FILE = "/.crosspoint/font_settings.bin";
  static constexpr uint8_t SETTINGS_VERSION = 2;  // Bumped for UI font support

  FontInfo _fonts[MAX_FONTS];
  int _fontCount = 0;
  int _selectedIndex = -1;    // -1 = built-in font (reader)
  int _selectedUiIndex = -1;  // -1 = fallback to reader font

  ExternalFont _activeFont;    // Reader font
  ExternalFont _activeUiFont;  // UI font
  bool _glyphCachesSuspended = false;

  bool isUiSharingReaderFont() const { return _selectedUiIndex >= 0 && _selectedUiIndex == _selectedIndex; }

  /**
   * Load selected reader font file
   */
  bool loadSelectedFont();

  /**
   * Load selected UI font file
   */
  bool loadSelectedUiFont();

  /// Writes (index, filename) for the font at `index` to `file`. Used by
  /// saveSettings() for both the reader and UI font slots.
  void writeFontChoice(HalFile& file, int index) const;

  /// Reads (savedIndex, savedFilename) from `file`, finds the matching font in
  /// the scanned list, sets `outIndex` and invokes `loader`. `label` is used
  /// for logging only ("reader" or "UI").
  void readFontChoice(HalFile& file, const char* label, int& outIndex, bool (FontManager::*loader)());
};

// Convenience macro
#define FontMgr FontManager::getInstance()
