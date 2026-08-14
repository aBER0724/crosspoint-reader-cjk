#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <string>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {

uint8_t fontSizeEnumFromSettings() {
  uint8_t value = SETTINGS.fontSize;
  if (value >= CrossPointSettings::FONT_SIZE_COUNT) value = CrossPointSettings::MEDIUM;
  return value;
}

struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};

constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};

bool hasCjkCoverage(const GfxRenderer& renderer, int fontId) {
  const auto fontIt = renderer.getFontMap().find(fontId);
  if (fontIt == renderer.getFontMap().end()) return false;

  for (const uint32_t codepoint : kCjkProbes) {
    if (fontIt->second.hasCodepoint(codepoint)) return true;
  }
  return false;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  ensureLoaded(renderer);
  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  if (previewActive_) {
    if (manager_.hasLoadedFonts()) manager_.unloadAll(renderer);
    loadedUiFamilyName_.clear();
    previewActive_ = false;
    residentFontsDirty_.store(true, std::memory_order_release);
  }

  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  const bool reloadRequested = residentFontsDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty - re-discovering fonts");
    registry_.discover();
  }

  std::string wantedReader = SETTINGS.sdFontFamilyName;
  std::string wantedUi = SETTINGS.sdUiFontFamilyName;
  const uint8_t sizeEnum = fontSizeEnumFromSettings();
  bool settingsChanged = false;

  const SdCardFontFamilyInfo* readerFamily = nullptr;
  const SdCardFontFileInfo* readerFile = nullptr;
  if (!wantedReader.empty()) {
    readerFamily = registry_.findFamily(wantedReader);
    if (readerFamily) readerFile = readerFamily->findClosestReaderSize(sizeEnum);
    if (!readerFamily || !readerFile) {
      LOG_ERR("SDFS", "Reader font family unavailable: %s (clearing)", wantedReader.c_str());
      SETTINGS.sdFontFamilyName[0] = '\0';
      wantedReader.clear();
      readerFamily = nullptr;
      readerFile = nullptr;
      settingsChanged = true;
    }
  }

  const SdCardFontFamilyInfo* uiFamily = nullptr;
  if (!wantedUi.empty()) {
    uiFamily = registry_.findFamily(wantedUi);
    if (!uiFamily) {
      LOG_ERR("SDFS", "UI font family unavailable: %s (clearing)", wantedUi.c_str());
      SETTINGS.sdUiFontFamilyName[0] = '\0';
      wantedUi.clear();
      settingsChanged = true;
    }
  }

  const uint8_t wantedReaderPointSize = readerFile ? readerFile->pointSize : 0;
  const bool configurationMatches = !reloadRequested && manager_.currentFamilyName() == wantedReader &&
                                    manager_.currentPointSize() == wantedReaderPointSize &&
                                    loadedUiFamilyName_ == wantedUi;
  if (configurationMatches) {
    if (settingsChanged) SETTINGS.saveToFile();
    return;
  }

  if (manager_.hasLoadedFonts()) manager_.unloadAll(renderer);
  loadedUiFamilyName_.clear();

  if (readerFamily) {
    if (manager_.loadFamily(*readerFamily, renderer, sizeEnum)) {
      LOG_DBG("SDFS", "Loaded reader font family: %s (%u pt)", wantedReader.c_str(), manager_.currentPointSize());
    } else {
      LOG_ERR("SDFS", "Failed to load reader font family: %s (clearing)", wantedReader.c_str());
      SETTINGS.sdFontFamilyName[0] = '\0';
      wantedReader.clear();
      readerFamily = nullptr;
      settingsChanged = true;
    }
  }

  if (uiFamily) {
    if (setupUiFallbacks(renderer, *uiFamily)) {
      loadedUiFamilyName_ = wantedUi;
      LOG_DBG("SDFS", "Loaded UI font family: %s", wantedUi.c_str());
    } else {
      LOG_ERR("SDFS", "No usable UI sizes in font family: %s (clearing)", wantedUi.c_str());
      SETTINGS.sdUiFontFamilyName[0] = '\0';
      wantedUi.clear();
      settingsChanged = true;
    }
  }

  if (settingsChanged) SETTINGS.saveToFile();
}
int SdCardFontSystem::beginPreview(GfxRenderer& renderer, const char* filePath, const char* familyName,
                                   uint8_t pointSize) {
  if (manager_.hasLoadedFonts()) manager_.unloadAll(renderer);
  loadedUiFamilyName_.clear();
  previewActive_ = false;

  SdCardFontFamilyInfo previewFamily;
  previewFamily.name = familyName;
  previewFamily.files.push_back({filePath, pointSize, 0});
  const int fontId = manager_.loadFamilyExtraSize(previewFamily, renderer, pointSize);
  if (fontId == 0) {
    residentFontsDirty_.store(true, std::memory_order_release);
    ensureLoaded(renderer);
    return 0;
  }

  previewActive_ = true;
  LOG_DBG("SDFS", "Loaded preview font: %s (%u pt)", familyName, pointSize);
  return fontId;
}

void SdCardFontSystem::endPreview(GfxRenderer& renderer) {
  if (!previewActive_) return;

  if (manager_.hasLoadedFonts()) manager_.unloadAll(renderer);
  loadedUiFamilyName_.clear();
  previewActive_ = false;
  residentFontsDirty_.store(true, std::memory_order_release);
  ensureLoaded(renderer);
}

bool SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer, const SdCardFontFamilyInfo& family) {
  bool loadedAny = false;
  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = manager_.loadFamilyExtraSize(family, renderer, ui.pointSize);
    if (sdFontId == 0) {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, family.name.c_str());
      continue;
    }
    if (!hasCjkCoverage(renderer, sdFontId)) {
      LOG_DBG("SDFS", "%s %u pt has no CJK coverage - skipping UI fallback", family.name.c_str(), ui.pointSize);
      continue;
    }

    renderer.setFallbackFont(ui.fontId, sdFontId);
    loadedAny = true;
  }
  return loadedAny;
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  return manager_.getFontId(familyName);
}
