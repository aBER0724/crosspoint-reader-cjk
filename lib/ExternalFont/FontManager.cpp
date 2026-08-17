#include "FontManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>

#include "FontFilenameParser.h"

// Out-of-class definitions for static constexpr members (required for ODR-use
// in C++14)
constexpr int FontManager::MAX_FONTS;
constexpr const char* FontManager::FONTS_DIR;
constexpr const char* FontManager::SETTINGS_FILE;
constexpr uint8_t FontManager::SETTINGS_VERSION;

FontManager& FontManager::getInstance() {
  static FontManager instance;
  return instance;
}

void FontManager::scanFonts() {
  HalFile dir = Storage.open(FONTS_DIR, O_RDONLY);
  if (!dir) {
    LOG_ERR("FONT_MGR", "Cannot open fonts directory: %s", FONTS_DIR);
    return;
  }

  if (!dir.isDirectory()) {
    LOG_ERR("FONT_MGR", "%s is not a directory", FONTS_DIR);
    dir.close();
    return;
  }

  char selectedReaderFilename[sizeof(FontInfo::filename)] = {};
  char selectedUiFilename[sizeof(FontInfo::filename)] = {};
  if (_selectedIndex >= 0 && _selectedIndex < _fontCount) {
    strncpy(selectedReaderFilename, _fonts[_selectedIndex].filename, sizeof(selectedReaderFilename) - 1);
  }
  if (_selectedUiIndex >= 0 && _selectedUiIndex < _fontCount) {
    strncpy(selectedUiFilename, _fonts[_selectedUiIndex].filename, sizeof(selectedUiFilename) - 1);
  }

  _fontCount = 0;
  HalFile entry;
  while (_fontCount < MAX_FONTS && (entry = dir.openNextFile())) {
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    char filename[64];
    entry.getName(filename, sizeof(filename));
    entry.close();

    // Try to parse the filename via the shared parser; skip unsupported names.
    ParsedFontFilename parsed;
    if (!parseFontFilename(filename, parsed)) {
      continue;
    }

    FontInfo& info = _fonts[_fontCount];
    strncpy(info.filename, filename, sizeof(info.filename) - 1);
    info.filename[sizeof(info.filename) - 1] = '\0';
    strncpy(info.name, parsed.name, sizeof(info.name) - 1);
    info.name[sizeof(info.name) - 1] = '\0';
    info.size = parsed.size;
    info.width = parsed.width;
    info.height = parsed.height;

    LOG_DBG("FONT_MGR", "Found font: %s (%dpt, %dx%d)", info.name, info.size, info.width, info.height);

    _fontCount++;
  }

  dir.close();

  const int oldReaderIndex = _selectedIndex;
  const int oldUiIndex = _selectedUiIndex;
  _selectedIndex = selectedReaderFilename[0] == '\0' ? -1 : findFontIndex(selectedReaderFilename);
  _selectedUiIndex = selectedUiFilename[0] == '\0' ? -1 : findFontIndex(selectedUiFilename);

  if (_selectedIndex < 0) {
    unloadReaderFont();
  }
  if (_selectedUiIndex < 0 || isUiSharingReaderFont()) {
    unloadUiFont();
  }

  if (_selectedIndex != oldReaderIndex || _selectedUiIndex != oldUiIndex) {
    saveSettings();
  }
  LOG_INF("FONT_MGR", "Scan complete: %d fonts found", _fontCount);
}

const FontInfo* FontManager::getFontInfo(int index) const {
  if (index < 0 || index >= _fontCount) {
    return nullptr;
  }
  return &_fonts[index];
}

int FontManager::findFontIndex(const char* filename) const {
  if (!filename || filename[0] == '\0') {
    return -1;
  }
  for (int i = 0; i < _fontCount; ++i) {
    if (strcmp(filename, _fonts[i].filename) == 0) {
      return i;
    }
  }
  return -1;
}

void FontManager::unloadReaderFont() {
  _activeFont.unload();
  _activeFontFilename[0] = '\0';
}

void FontManager::unloadUiFont() {
  _activeUiFont.unload();
  _activeUiFontFilename[0] = '\0';
}

bool FontManager::loadSelectedFont() {
  unloadReaderFont();

  if (_selectedIndex < 0 || _selectedIndex >= _fontCount) {
    return false;
  }

  char filepath[80];
  snprintf(filepath, sizeof(filepath), "%s/%s", FONTS_DIR, _fonts[_selectedIndex].filename);

  const bool loaded = _activeFont.load(filepath);
  if (loaded) {
    strncpy(_activeFontFilename, _fonts[_selectedIndex].filename, sizeof(_activeFontFilename) - 1);
    _activeFontFilename[sizeof(_activeFontFilename) - 1] = '\0';
  }
  if (isUiSharingReaderFont()) {
    unloadUiFont();
  }
  return loaded;
}

bool FontManager::loadSelectedUiFont() {
  if (_selectedUiIndex < 0 || _selectedUiIndex >= _fontCount) {
    unloadUiFont();
    return false;
  }

  if (isUiSharingReaderFont()) {
    unloadUiFont();
    if (!isReaderSelectionReady(_selectedIndex)) {
      return loadSelectedFont();
    }
    return true;
  }

  unloadUiFont();

  char filepath[80];
  snprintf(filepath, sizeof(filepath), "%s/%s", FONTS_DIR, _fonts[_selectedUiIndex].filename);

  const bool loaded = _activeUiFont.load(filepath);
  if (loaded) {
    strncpy(_activeUiFontFilename, _fonts[_selectedUiIndex].filename, sizeof(_activeUiFontFilename) - 1);
    _activeUiFontFilename[sizeof(_activeUiFontFilename) - 1] = '\0';
  }
  return loaded;
}

bool FontManager::isReaderSelectionReady(const int index) const {
  if (index < 0) {
    return !_activeFont.isLoaded();
  }
  return isValidSelectionIndex(index) && _activeFont.isLoaded() &&
         strcmp(_activeFontFilename, _fonts[index].filename) == 0;
}

bool FontManager::isUiSelectionReady(const int readerIndex, const int uiIndex) const {
  if (uiIndex < 0) {
    return !_activeUiFont.isLoaded();
  }
  if (!isValidSelectionIndex(uiIndex)) {
    return false;
  }
  if (readerIndex == uiIndex) {
    return isReaderSelectionReady(readerIndex) && !_activeUiFont.isLoaded();
  }
  return _activeUiFont.isLoaded() && strcmp(_activeUiFontFilename, _fonts[uiIndex].filename) == 0;
}

bool FontManager::applyFontSelection(const int readerIndex, const int uiIndex) {
  if (!isValidSelectionIndex(readerIndex) || !isValidSelectionIndex(uiIndex)) {
    return false;
  }

  _selectedIndex = readerIndex;
  _selectedUiIndex = uiIndex;

  if (_selectedIndex >= 0) {
    if (!isReaderSelectionReady(_selectedIndex) && !loadSelectedFont()) {
      return false;
    }
  } else {
    unloadReaderFont();
  }

  if (_selectedUiIndex >= 0) {
    if (!isUiSelectionReady(_selectedIndex, _selectedUiIndex) && !loadSelectedUiFont()) {
      return false;
    }
  } else {
    unloadUiFont();
  }

  return true;
}

bool FontManager::restoreAvailableSelection(const int readerIndex, const int uiIndex) {
  const bool readerIndexValid = isValidSelectionIndex(readerIndex);
  const bool uiIndexValid = isValidSelectionIndex(uiIndex);
  const int validReaderIndex = readerIndexValid ? readerIndex : -1;
  const int validUiIndex = uiIndexValid ? uiIndex : -1;

  if (readerIndexValid && uiIndexValid && applyFontSelection(readerIndex, uiIndex)) {
    return true;
  }

  // A failed apply can leave the requested indices installed before one of
  // the font loads fails. Rebuild from a known state, preserving the Reader
  // slot first because it is required for book content.
  useBuiltinFonts();
  bool readerAvailable = true;
  if (validReaderIndex >= 0 && !applyFontSelection(validReaderIndex, -1)) {
    useBuiltinFonts();
    readerAvailable = false;
  }

  if (validUiIndex >= 0 && readerAvailable) {
    if (applyFontSelection(validReaderIndex, validUiIndex)) {
      return readerIndexValid && uiIndexValid;
    }

    // Loading the UI slot may have failed after the Reader slot succeeded.
    // Restore a coherent Reader-only state before trying any UI-only fallback.
    if (applyFontSelection(validReaderIndex, -1)) {
      return false;
    }
    useBuiltinFonts();
    readerAvailable = false;
  }

  if (validUiIndex >= 0 && !readerAvailable && applyFontSelection(-1, validUiIndex)) {
    return readerIndexValid && readerIndex == -1 && uiIndexValid;
  }

  if (readerAvailable) {
    return readerIndexValid && uiIndexValid && uiIndex == -1;
  }

  useBuiltinFonts();
  return false;
}

void FontManager::useBuiltinFonts() {
  _selectedIndex = -1;
  _selectedUiIndex = -1;
  unloadReaderFont();
  unloadUiFont();
}

bool FontManager::selectFonts(const int readerIndex, const int uiIndex) {
  if (!isValidSelectionIndex(readerIndex) || !isValidSelectionIndex(uiIndex)) {
    LOG_ERR("FONT_MGR", "Invalid font selection: reader=%d, UI=%d", readerIndex, uiIndex);
    return false;
  }

  const int oldReaderIndex = _selectedIndex;
  const int oldUiIndex = _selectedUiIndex;
  if (readerIndex == oldReaderIndex && uiIndex == oldUiIndex && isReaderSelectionReady(readerIndex) &&
      isUiSelectionReady(readerIndex, uiIndex)) {
    return true;
  }

  if (applyFontSelection(readerIndex, uiIndex)) {
    saveSettings();
    return true;
  }

  LOG_ERR("FONT_MGR", "Failed to load font selection: reader=%d, UI=%d; restoring previous selection", readerIndex,
          uiIndex);
  if (!restoreAvailableSelection(oldReaderIndex, oldUiIndex)) {
    LOG_ERR("FONT_MGR", "Previous font selection was only partially recoverable");
    saveSettings();
  }
  return false;
}

bool FontManager::selectFont(const int index) {
  const int uiIndex = isValidSelectionIndex(_selectedUiIndex) ? _selectedUiIndex : -1;
  return selectFonts(index, uiIndex);
}

bool FontManager::selectUiFont(const int index) {
  const int readerIndex = isValidSelectionIndex(_selectedIndex) ? _selectedIndex : -1;
  return selectFonts(readerIndex, index);
}

bool FontManager::clearSelections(const bool reader, const bool ui) {
  if (!reader && !ui) {
    return true;
  }

  const int readerIndex = reader ? -1 : (isValidSelectionIndex(_selectedIndex) ? _selectedIndex : -1);
  const int uiIndex = ui ? -1 : (isValidSelectionIndex(_selectedUiIndex) ? _selectedUiIndex : -1);
  return selectFonts(readerIndex, uiIndex);
}

bool FontManager::previewFont(int index) {
  const int oldReaderIndex = _selectedIndex;
  const int oldUiIndex = _selectedUiIndex;
  if (applyFontSelection(index, oldUiIndex)) {
    return true;
  }
  if (!restoreAvailableSelection(oldReaderIndex, oldUiIndex)) {
    saveSettings();
  }
  return false;
}

bool FontManager::previewUiFont(int index) {
  const int oldReaderIndex = _selectedIndex;
  const int oldUiIndex = _selectedUiIndex;
  if (applyFontSelection(oldReaderIndex, index)) {
    return true;
  }
  if (!restoreAvailableSelection(oldReaderIndex, oldUiIndex)) {
    saveSettings();
  }
  return false;
}

bool FontManager::restoreFontSelection(int readerIndex, int uiIndex) {
  if (restoreAvailableSelection(readerIndex, uiIndex)) {
    return true;
  }

  LOG_ERR("FONT_MGR", "Requested font selection was only partially recoverable: reader=%d, UI=%d", readerIndex,
          uiIndex);
  saveSettings();
  return false;
}

ExternalFont* FontManager::getActiveFont() {
  if (_selectedIndex >= 0 && _activeFont.isLoaded()) {
    return &_activeFont;
  }
  return nullptr;
}

ExternalFont* FontManager::getActiveUiFont() {
  if (_selectedUiIndex < 0) {
    return nullptr;
  }
  if (isUiSharingReaderFont()) {
    return _activeFont.isLoaded() ? &_activeFont : nullptr;
  }
  if (_activeUiFont.isLoaded()) {
    return &_activeUiFont;
  }
  return nullptr;
}

void FontManager::releaseGlyphCaches() {
  _activeFont.releaseGlyphCache();
  if (!isUiSharingReaderFont()) {
    _activeUiFont.releaseGlyphCache();
  }
}

void FontManager::setGlyphCachesSuspended(bool suspended) {
  _glyphCachesSuspended = suspended;
  if (suspended) {
    _activeFont.releaseGlyphCache();
  }
}

void FontManager::setUiGlyphCacheSuspended(bool suspended) {
  if (_uiGlyphCacheSuspended == suspended) {
    if (suspended && !isUiSharingReaderFont()) {
      _activeUiFont.releaseGlyphCache();
    }
    return;
  }
  _uiGlyphCacheSuspended = suspended;
  if (suspended && !isUiSharingReaderFont()) {
    _activeUiFont.releaseGlyphCache();
    // Entering reader: pre-allocate the reader glyph cache while the heap is
    // cleanest (UI cache just freed). Mirrors the exit branch. Without this,
    // the cache is allocated lazily on the first getGlyph() during render,
    // where heap fragmentation can make the ~36KB contiguous new[] fail once
    // and set the sticky _glyphCacheAllocationFailed flag -- ensureGlyphCache()
    // then bails before the releaseGlyphCache() that would clear it, bricking
    // the reader font for the whole session (blank/missing CJK, slow fallback).
    _activeFont.releaseGlyphCache();
    _activeFont.prepareGlyphCache();
  } else if (!suspended && !isUiSharingReaderFont()) {
    // Release the reader bitmap cache before UI drawing resumes so the
    // independent UI font can immediately claim one contiguous cache block
    // before home cover/title allocations fragment the heap.
    _activeFont.releaseGlyphCache();
    _activeUiFont.prepareGlyphCache();
  }
}

bool FontManager::isGlyphCacheSuspendedFor(const ExternalFont* font) const {
  if (!font) {
    return false;
  }
  if (_glyphCachesSuspended && !isUiSharingReaderFont() && font == &_activeFont) {
    return true;
  }
  return _uiGlyphCacheSuspended && !isUiSharingReaderFont() && font == &_activeUiFont;
}

FontManager::ScopedGlyphCacheSuspension::ScopedGlyphCacheSuspension(FontManager& manager)
    : _manager(manager), _previous(manager.areGlyphCachesSuspended()) {
  _manager.setGlyphCachesSuspended(true);
}

FontManager::ScopedGlyphCacheSuspension::~ScopedGlyphCacheSuspension() { _manager.setGlyphCachesSuspended(_previous); }

void FontManager::writeFontChoice(HalFile& file, const int index) const {
  serialization::writePod(file, index);
  if (index >= 0 && index < _fontCount) {
    serialization::writeString(file, std::string(_fonts[index].filename));
  } else {
    serialization::writeString(file, std::string(""));
  }
}

bool FontManager::readFontChoice(HalFile& file, const char* label, int& outIndex) {
  outIndex = -1;
  int savedIndex = -1;
  serialization::readPod(file, savedIndex);

  std::string savedFilename;
  serialization::readString(file, savedFilename);

  if (savedFilename.empty()) {
    return savedIndex != -1;
  }

  for (int i = 0; i < _fontCount; i++) {
    if (savedFilename == _fonts[i].filename) {
      outIndex = i;
      LOG_INF("FONT_MGR", "Found saved %s font: %s", label, savedFilename.c_str());
      return savedIndex != i;
    }
  }
  LOG_ERR("FONT_MGR", "Saved %s font not found: %s", label, savedFilename.c_str());
  return true;
}

void FontManager::saveSettings() {
  Storage.mkdir("/.crosspoint");

  HalFile file;
  if (!Storage.openFileForWrite("FONT_MGR", SETTINGS_FILE, file)) {
    LOG_ERR("FONT_MGR", "Failed to save settings");
    return;
  }

  serialization::writePod(file, SETTINGS_VERSION);
  writeFontChoice(file, _selectedIndex);
  // UI font slot (version 2+).
  writeFontChoice(file, _selectedUiIndex);

  file.close();
  LOG_DBG("FONT_MGR", "Settings saved");
}

void FontManager::loadSettings() {
  HalFile file;
  if (!Storage.openFileForRead("FONT_MGR", SETTINGS_FILE, file)) {
    LOG_DBG("FONT_MGR", "No settings file, using defaults");
    return;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version < 1 || version > SETTINGS_VERSION) {
    LOG_ERR("FONT_MGR", "Settings version mismatch (%d vs %d)", version, SETTINGS_VERSION);
    file.close();
    return;
  }

  int requestedReaderIndex = -1;
  int requestedUiIndex = -1;
  bool settingsChanged = readFontChoice(file, "reader", requestedReaderIndex);

  // UI font slot (version 2+).
  if (version >= 2) {
    settingsChanged = readFontChoice(file, "UI", requestedUiIndex) || settingsChanged;
  }

  file.close();

  if (!restoreAvailableSelection(requestedReaderIndex, requestedUiIndex)) {
    LOG_ERR("FONT_MGR", "Saved font selection was only partially recoverable");
    settingsChanged = true;
  }

  if (settingsChanged) {
    saveSettings();
  }
}
