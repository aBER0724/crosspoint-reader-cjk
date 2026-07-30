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
  _fontCount = 0;

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
  LOG_INF("FONT_MGR", "Scan complete: %d fonts found", _fontCount);
}

const FontInfo* FontManager::getFontInfo(int index) const {
  if (index < 0 || index >= _fontCount) {
    return nullptr;
  }
  return &_fonts[index];
}

bool FontManager::loadSelectedFont() {
  _activeFont.unload();

  if (_selectedIndex < 0 || _selectedIndex >= _fontCount) {
    return false;
  }

  char filepath[80];
  snprintf(filepath, sizeof(filepath), "%s/%s", FONTS_DIR, _fonts[_selectedIndex].filename);

  const bool loaded = _activeFont.load(filepath);
  if (isUiSharingReaderFont()) {
    _activeUiFont.unload();
  }
  return loaded;
}

bool FontManager::loadSelectedUiFont() {
  if (_selectedUiIndex < 0 || _selectedUiIndex >= _fontCount) {
    _activeUiFont.unload();
    return false;
  }

  if (isUiSharingReaderFont()) {
    _activeUiFont.unload();
    if (!_activeFont.isLoaded()) {
      return loadSelectedFont();
    }
    return true;
  }

  _activeUiFont.unload();

  char filepath[80];
  snprintf(filepath, sizeof(filepath), "%s/%s", FONTS_DIR, _fonts[_selectedUiIndex].filename);

  return _activeUiFont.load(filepath);
}

void FontManager::selectFont(int index) {
  if (index == _selectedIndex) {
    return;
  }

  _selectedIndex = index;

  if (index >= 0) {
    loadSelectedFont();
  } else {
    _activeFont.unload();
  }

  saveSettings();
}

void FontManager::selectUiFont(int index) {
  if (index == _selectedUiIndex) {
    return;
  }

  _selectedUiIndex = index;

  if (index >= 0) {
    loadSelectedUiFont();
  } else {
    _activeUiFont.unload();
  }

  saveSettings();
}

bool FontManager::previewFont(int index) {
  _selectedIndex = index;

  if (index >= 0) {
    return loadSelectedFont();
  }

  _activeFont.unload();
  return true;
}

bool FontManager::previewUiFont(int index) {
  _selectedUiIndex = index;

  if (index >= 0) {
    return loadSelectedUiFont();
  }

  _activeUiFont.unload();
  return true;
}

void FontManager::restoreFontSelection(int readerIndex, int uiIndex) {
  _selectedIndex = readerIndex;
  _selectedUiIndex = uiIndex;

  if (_selectedIndex >= 0) {
    loadSelectedFont();
  } else {
    _activeFont.unload();
  }

  if (_selectedUiIndex >= 0) {
    loadSelectedUiFont();
  } else {
    _activeUiFont.unload();
  }
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

bool FontManager::isGlyphCacheSuspendedFor(const ExternalFont* font) const {
  if (!_glyphCachesSuspended || !font) {
    return false;
  }
  if (isUiSharingReaderFont()) {
    return false;
  }
  return font == &_activeFont;
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

void FontManager::readFontChoice(HalFile& file, const char* label, int& outIndex, bool (FontManager::*loader)()) {
  int savedIndex = -1;
  serialization::readPod(file, savedIndex);

  std::string savedFilename;
  serialization::readString(file, savedFilename);

  if (savedIndex < 0 || savedFilename.empty()) {
    return;
  }

  for (int i = 0; i < _fontCount; i++) {
    if (savedFilename == _fonts[i].filename) {
      outIndex = i;
      (this->*loader)();
      LOG_INF("FONT_MGR", "Restored %s font: %s", label, savedFilename.c_str());
      return;
    }
  }
  LOG_ERR("FONT_MGR", "Saved %s font not found: %s", label, savedFilename.c_str());
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

  readFontChoice(file, "reader", _selectedIndex, &FontManager::loadSelectedFont);

  // UI font slot (version 2+).
  if (version >= 2) {
    readFontChoice(file, "UI", _selectedUiIndex, &FontManager::loadSelectedUiFont);
  }

  file.close();
}
