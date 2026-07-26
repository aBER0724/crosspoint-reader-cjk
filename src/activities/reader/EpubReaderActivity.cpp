#include "EpubReaderActivity.h"

#include <Arduino.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ReaderOperationTrace.h>
#include <ReaderRuntimePolicy.h>

#include <algorithm>
#include <cstdint>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "activities/settings/FontSelectActivity.h"
#include "activities/settings/LineSpacingSelectionActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr uint32_t adjacentGlyphPreloadMinHeap = ReaderRuntime::MemoryThresholds::optionalQualityHeap;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

uint16_t clampDurationMs(const unsigned long duration) {
  return duration > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(duration);
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Screen orientation (both renderer and input) is already set by
  // ActivityManager → OrientationHelper::applyOrientation() before onEnter().

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // ActivityManager applies the next activity's orientation on Pop/Replace,
  // so we don't need to manually reset the renderer here.

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  // Skip button processing after returning from subactivity
  if (skipNextButtonCheck) {
    const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                             !mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (confirmCleared && backCleared) {
      skipNextButtonCheck = false;
    }
    return;
  }

  // Handle pending go home (e.g. from menu GO_HOME action)
  if (pendingGoHome) {
    pendingGoHome = false;
    onGoHome();
    return;
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showReaderMenu();
    return;
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoHome();
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    activityManager.goHome();
    return;
  }

  // When long-press chapter skip is disabled, turn pages on press instead of release.
  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
  const bool prevTriggered = usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasPressed(MappedInputManager::Button::Left))
                                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasReleased(MappedInputManager::Button::Left));
  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool nextTriggered = usePressForPageTurn
                                 ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasPressed(MappedInputManager::Button::Right))
                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasReleased(MappedInputManager::Button::Right));

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  const auto now = millis();
  if (now - lastPageTurnMs <= 900) {
    consecutivePageTurns = std::min<uint8_t>(consecutivePageTurns + 1, 20);
  } else {
    consecutivePageTurns = 1;
  }
  lastPageTurnMs = now;
  lastPageTurnDirection = nextTriggered ? 1 : -1;

  // any button press when at end of the book goes back to the last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = UINT16_MAX;
    pendingStrongRefresh = true;
    requestUpdate();
    return;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      pendingStrongRefresh = true;
      section.reset();
    }
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      {
        RenderLock lock;
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        pendingStrongRefresh = true;
        section.reset();
      }
    }
    requestUpdate();
  } else {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      {
        RenderLock lock;
        nextPageNumber = 0;
        currentSpineIndex++;
        pendingStrongRefresh = true;
        section.reset();
      }
    }
    requestUpdate();
  }
}

void EpubReaderActivity::showReaderMenu() {
  if (!epub) {
    return;
  }

  // Snapshot reader state under render lock.
  int menuSpineIndex = 0;
  int menuCurrentPage = 0;
  int menuTotalPages = 0;
  {
    RenderLock lock;
    menuSpineIndex = currentSpineIndex;
    if (section) {
      menuCurrentPage = section->currentPage + 1;
      menuTotalPages = section->pageCount;
    }
  }

  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && menuTotalPages > 0) {
    const float chapterProgress = static_cast<float>(menuCurrentPage - 1) / static_cast<float>(menuTotalPages);
    bookProgress = epub->calculateProgress(menuSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, epub->getTitle(), menuCurrentPage, menuTotalPages,
                                               bookProgressPercent, SETTINGS.orientation),
      [this](const ActivityResult& result) { handleMenuResult(result); });
}

void EpubReaderActivity::onSelectChapter() {
  const int spineIdx = currentSpineIndex;
  const std::string path = epub->getPath();

  startActivityForResult(
      std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
      [this](const ActivityResult& chapterResult) {
        if (chapterResult.isCancelled) {
          // User pressed back from chapter selection, re-show the reader menu.
          showReaderMenu();
          return;
        }
        auto* chapterData = std::get_if<ChapterResult>(&chapterResult.data);
        if (chapterData) {
          if (currentSpineIndex != chapterData->spineIndex) {
            currentSpineIndex = chapterData->spineIndex;
            nextPageNumber = 0;
            pendingStrongRefresh = true;
            section.reset();
          }
        }
        skipNextButtonCheck = true;
        requestUpdate();
      });
}

void EpubReaderActivity::onGoToPercent() {
  float bookProgress = 0.0f;
  if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

  startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
                         [this](const ActivityResult& percentResult) {
                           if (!percentResult.isCancelled) {
                             auto* percentData = std::get_if<PercentResult>(&percentResult.data);
                             if (percentData) {
                               jumpToPercent(percentData->percent);
                             }
                           }
                           skipNextButtonCheck = true;
                           requestUpdate();
                         });
}

void EpubReaderActivity::onReaderSettings() {
  // Use startActivityForResult so we get a callback when settings close,
  // allowing us to properly invalidate section and re-render.
  invalidateSectionPreservingPosition();
  skipNextButtonCheck = true;
  startActivityForResult(std::make_unique<SettingsActivity>(renderer, mappedInput, 1, 1),
                         [this](const ActivityResult&) {
                           invalidateSectionPreservingPosition();
                           skipNextButtonCheck = true;
                           requestUpdate();
                         });
}

void EpubReaderActivity::onToggleFirstLineIndent() {
  SETTINGS.firstLineIndent = !SETTINGS.firstLineIndent;
  SETTINGS.saveToFile();
  invalidateSectionPreservingPosition();
  skipNextButtonCheck = true;
  requestUpdate();
}

void EpubReaderActivity::onToggleInvertImages() {
  SETTINGS.invertImages = !SETTINGS.invertImages;
  SETTINGS.saveToFile();
  renderer.setInvertImagesInDarkMode(SETTINGS.invertImages);
  pendingStrongRefresh = true;
  skipNextButtonCheck = true;
  requestUpdate();
}

void EpubReaderActivity::onChangeFontFamily() {
  invalidateSectionPreservingPosition();
  skipNextButtonCheck = true;
  // Push FontSelectActivity; we'll invalidate section when it returns.
  startActivityForResult(
      std::make_unique<FontSelectActivity>(renderer, mappedInput, FontSelectActivity::SelectMode::Reader),
      [this](const ActivityResult&) {
        invalidateSectionPreservingPosition();
        skipNextButtonCheck = true;
        requestUpdate();
      });
}

void EpubReaderActivity::onChangeLineSpacing() {
  startActivityForResult(
      std::make_unique<LineSpacingSelectionActivity>(renderer, mappedInput, static_cast<int>(SETTINGS.lineSpacing)),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          auto* data = std::get_if<PercentResult>(&result.data);
          if (data) {
            SETTINGS.lineSpacing = static_cast<uint8_t>(data->percent);
            SETTINGS.saveToFile();
          }
        }
        invalidateSectionPreservingPosition();
        skipNextButtonCheck = true;
        requestUpdate();
      });
}

void EpubReaderActivity::onStatusBarSettings() {
  startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) {
                           pendingStrongRefresh = true;
                           skipNextButtonCheck = true;
                           requestUpdate();
                         });
}

void EpubReaderActivity::onDisplayQr() {
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    auto p = section->loadPageFromSectionFile();
    if (p) {
      std::string fullText;
      for (const auto& el : p->elements) {
        if (el->getTag() == TAG_PageLine) {
          const auto& line = static_cast<const PageLine&>(*el);
          if (line.getBlock()) {
            const auto& words = line.getBlock()->getWords();
            for (const auto& w : words) {
              if (!fullText.empty()) fullText += " ";
              fullText += w;
            }
          }
        }
      }
      if (!fullText.empty()) {
        startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                               [this](const ActivityResult&) {
                                 skipNextButtonCheck = true;
                                 requestUpdate();
                               });
        return;
      }
    }
  }
  // If no text or page loading failed, just resume reading
  skipNextButtonCheck = true;
  requestUpdate();
}

void EpubReaderActivity::onGoHome() { pendingGoHome = true; }

void EpubReaderActivity::onDeleteCache() {
  {
    RenderLock lock;
    if (epub) {
      uint16_t backupSpine = currentSpineIndex;
      uint16_t backupPage = section->currentPage;
      uint16_t backupPageCount = section->pageCount;

      section.reset();
      epub->clearCache();
      epub->setupCacheDir();
      saveProgress(backupSpine, backupPage, backupPageCount);
    }
  }
  pendingGoHome = true;
}

void EpubReaderActivity::onScreenshot() {
  {
    RenderLock lock;
    pendingScreenshot = true;
  }
  skipNextButtonCheck = true;
  requestUpdate();
}

void EpubReaderActivity::onSync() {
  if (!KOREADER_STORE.hasCredentials()) {
    return;
  }
  const int currentPage = section ? section->currentPage : 0;
  const int totalPages = section ? section->pageCount : 0;
  startActivityForResult(
      std::make_unique<KOReaderSyncActivity>(renderer, mappedInput, epub, epub->getPath(), currentSpineIndex,
                                             currentPage, totalPages),
      [this](const ActivityResult& syncResult) {
        auto* syncData = std::get_if<SyncResult>(&syncResult.data);
        if (syncData) {
          if (currentSpineIndex != syncData->spineIndex || (section && section->currentPage != syncData->page)) {
            currentSpineIndex = syncData->spineIndex;
            nextPageNumber = syncData->page;
            section.reset();
          }
        }
        skipNextButtonCheck = true;
        requestUpdate();
      });
}

void EpubReaderActivity::handleMenuResult(const ActivityResult& result) {
  // Apply pending orientation from the menu (for both Back and Confirm actions).
  auto* menuResult = std::get_if<MenuResult>(&result.data);
  if (menuResult) {
    applyOrientation(menuResult->orientation);
  }

  if (result.isCancelled) {
    // Back pressed — orientation already applied above, just resume reading.
    skipNextButtonCheck = true;
    requestUpdate();
    return;
  }

  if (!menuResult) {
    skipNextButtonCheck = true;
    requestUpdate();
    return;
  }

  const auto action = static_cast<EpubReaderMenuActivity::MenuAction>(menuResult->action);

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER:
      onSelectChapter();
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT:
      onGoToPercent();
      break;
    case EpubReaderMenuActivity::MenuAction::READER_SETTINGS:
      onReaderSettings();
      break;
    case EpubReaderMenuActivity::MenuAction::STYLE_FIRST_LINE_INDENT:
      onToggleFirstLineIndent();
      break;
    case EpubReaderMenuActivity::MenuAction::STYLE_INVERT_IMAGES:
      onToggleInvertImages();
      break;
    case EpubReaderMenuActivity::MenuAction::STYLE_FONT_FAMILY:
      onChangeFontFamily();
      break;
    case EpubReaderMenuActivity::MenuAction::STYLE_LINE_SPACING:
      onChangeLineSpacing();
      break;
    case EpubReaderMenuActivity::MenuAction::STYLE_STATUS_BAR:
      onStatusBarSettings();
      break;
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR:
      onDisplayQr();
      break;
    case EpubReaderMenuActivity::MenuAction::GO_HOME:
      onGoHome();
      break;
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE:
      onDeleteCache();
      break;
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT:
      onScreenshot();
      break;
    case EpubReaderMenuActivity::MenuAction::SYNC:
      onSync();
      break;
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock;
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer and input orientation to match the new coordinate system.
    OrientationHelper::applyOrientation(renderer, mappedInput, this);

    // Reset section to force re-layout in the new orientation.
    pendingStrongRefresh = true;
    section.reset();
  }
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom +=
      std::max(SETTINGS.screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  if (!section) {
    if (!loadOrBuildSection(orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft)) {
      return;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    return;
  }

  {
    const auto loadStart = millis();
    auto p = section->loadPageFromSectionFile();
    const auto loadMs = clampDurationMs(millis() - loadStart);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      if (sectionLoadRetryCount == 0) {
        sectionLoadRetryCount++;
        pendingStrongRefresh = true;
        section->clearCache();
        section.reset();
        requestUpdate();
        return;
      }
      sectionLoadRetryCount = 0;
      section.reset();
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
      if (renderer.isDarkMode()) {
        renderer.displayBufferDarkRedrive();
      } else {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      return;
    }
    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft,
                   loadMs);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    renderer.getFontCacheManager()->clearCache();
    sectionLoadRetryCount = 0;
  }
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  preloadAdjacentPageGlyphs();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void EpubReaderActivity::preloadAdjacentPageGlyphs() {
  if (!section || !FontManager::getInstance().isExternalFontEnabled()) {
    return;
  }

  if (ESP.getFreeHeap() < adjacentGlyphPreloadMinHeap) {
    return;
  }

  const int currentPage = section->currentPage;
  const int targetPage = currentPage + lastPageTurnDirection;
  if (targetPage < 0 || targetPage >= section->pageCount) {
    return;
  }

  ExternalFont* extFont = FontManager::getInstance().getActiveFont();
  if (!extFont) {
    return;
  }

  const auto preloadStart = millis();
  section->currentPage = targetPage;
  auto adjacentPage = section->loadPageFromSectionFile();
  section->currentPage = currentPage;
  if (!adjacentPage) {
    return;
  }

  std::vector<uint32_t> codepoints;
  adjacentPage->collectCodepoints(codepoints, extFont->getPreloadLimit());
  if (codepoints.empty()) {
    return;
  }

  extFont->preloadGlyphs(codepoints.data(), codepoints.size());
  LOG_DBG("ERS", "Prefetched page %d glyphs in %ums", targetPage + 1, clampDurationMs(millis() - preloadStart));
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  FsFile f;
  if (Storage.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", spineIndex, currentPage);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }
}
bool EpubReaderActivity::loadOrBuildSection(const int orientedMarginTop, const int orientedMarginRight,
                                            const int orientedMarginBottom, const int orientedMarginLeft) {
  const auto filepath = epub->getSpineItem(currentSpineIndex).href;
  LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
  section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  const float lineCompression = SETTINGS.getReaderLineCompression();
  LOG_DBG("ERS", "Reflow params: lineSpacing=%u, compression=%.2f, viewport=%ux%u", SETTINGS.lineSpacing,
          lineCompression, viewportWidth, viewportHeight);

  lastSectionCacheRebuilt = false;
  if (!section->loadSectionFile(SETTINGS.getReaderFontId(), lineCompression, SETTINGS.extraParagraphSpacing,
                                SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                SETTINGS.firstLineIndent, SETTINGS.embeddedStyle, 0)) {
    LOG_DBG("ERS", "Cache not found, building...");

    FontManager::ScopedGlyphCacheSuspension glyphCacheSuspension(FontManager::getInstance());
    LOG_DBG("ERS", "Suspended external glyph caches before section build, free heap: %u", ESP.getFreeHeap());

    if (ReaderRuntime::classifyReaderMemory(ESP.getFreeHeap()) == ReaderRuntime::MemoryDecision::Stop) {
      LOG_ERR("ERS", "Insufficient heap before section build: %u", ESP.getFreeHeap());
      section.reset();
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
      if (renderer.isDarkMode()) {
        renderer.displayBufferDarkRedrive();
      } else {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      return false;
    }

    const auto popupFn = std::function<void()>([this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); });

    if (!section->createSectionFile(SETTINGS.getReaderFontId(), lineCompression, SETTINGS.extraParagraphSpacing,
                                    SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                    SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, 0, SETTINGS.firstLineIndent,
                                    popupFn)) {
      LOG_ERR("ERS", "Failed to persist page data to SD");
      section.reset();
      // Show error to user instead of silent return
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return false;
    }
    lastSectionCacheRebuilt = true;
    pendingStrongRefresh = true;
  } else {
    LOG_DBG("ERS", "Cache found, skipping build...");
  }

  if (nextPageNumber == UINT16_MAX) {
    section->currentPage = section->pageCount - 1;
  } else {
    section->currentPage = nextPageNumber;
  }

  // handles changes in reader settings and reset to approximate position based on cached progress
  if (cachedChapterTotalPageCount > 0) {
    // only goes to relative position if spine index matches cached value
    if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      const int newPage = static_cast<int>(progress * section->pageCount);
      section->currentPage = newPage;
    }
    cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
  }

  if (pendingPercentJump && section->pageCount > 0) {
    // Apply the pending percent jump now that we know the new section's page count.
    int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
    if (newPage >= section->pageCount) {
      newPage = section->pageCount - 1;
    }
    section->currentPage = newPage;
    pendingPercentJump = false;
  }
  return true;
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft, const uint16_t loadMs) {
  const auto totalStart = millis();
  uint16_t preloadMs = 0;
  uint16_t renderMs = 0;
  uint16_t displayMs = 0;
  uint16_t grayscaleMs = 0;

  // Preload external font glyphs: collect codepoints from page, sort them,
  // and batch-read from SD sequentially. Much faster than random reads during render.
  FontManager& fm = FontManager::getInstance();
  if (fm.isExternalFontEnabled()) {
    const auto preloadStart = millis();
    ExternalFont* extFont = fm.getActiveFont();
    if (extFont) {
      std::vector<uint32_t> codepoints;
      page->collectCodepoints(codepoints, extFont->getPreloadLimit());
      if (!codepoints.empty()) {
        extFont->preloadGlyphs(codepoints.data(), codepoints.size());
      }
    }
    preloadMs = clampDurationMs(millis() - preloadStart);
  }

  // Force special handling for pages with images when anti-aliasing is on
  const auto renderStart = millis();
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  renderMs = clampDurationMs(millis() - renderStart);

  ReaderRuntime::RefreshContext refreshContext{};
  refreshContext.readerKind = ReaderRuntime::ReaderKind::Epub;
  refreshContext.darkMode = renderer.isDarkMode();
  refreshContext.containsImages = page->hasImages();
  refreshContext.textAntiAliasing = SETTINGS.textAntiAliasing;
  refreshContext.externalFontEnabled = fm.isExternalFontEnabled();
  refreshContext.grayscaleRequested = SETTINGS.textAntiAliasing;
  refreshContext.chapterBoundary = pendingStrongRefresh;
  refreshContext.cacheRebuilt = lastSectionCacheRebuilt;
  refreshContext.lowMemory =
      ReaderRuntime::classifyReaderMemory(ESP.getFreeHeap()) != ReaderRuntime::MemoryDecision::Proceed;
  refreshContext.cadenceRemaining = pagesUntilFullRefresh;
  refreshContext.refreshFrequency = SETTINGS.getRefreshFrequency();
  refreshContext.consecutiveTurns = consecutivePageTurns;

  auto decision = ReaderRuntime::chooseReaderRefresh(refreshContext);

  const auto displayStart = millis();
  if (decision.useImageDoubleFast) {
    // Double refresh with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      if (renderer.isDarkMode()) {
        renderer.displayBufferDarkRedrive();
      } else {
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }

      // Re-render page content to restore images into the blanked area
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderStatusBar();
      if (renderer.isDarkMode()) {
        renderer.displayBufferDarkRedrive();
      } else {
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
    } else {
      ReaderUtils::displayWithRefreshDecision(renderer, decision);
    }
  } else {
    ReaderUtils::displayWithRefreshDecision(renderer, decision);
  }
  displayMs = clampDurationMs(millis() - displayStart);
  pagesUntilFullRefresh = decision.nextCadenceRemaining;

  if (decision.runGrayscalePass) {
    const auto grayscaleStart = millis();
    if (renderer.storeBwBuffer()) {
      // Disable dark mode during grayscale passes. The e-ink grayscale LUT tables
      // are designed for white-background rendering. During grayscale passes:
      // - clearScreen(0x00) fills the buffer with all-zeros (black pixels)
      // - Font anti-aliasing is rendered as white marks on a black buffer
      // This produces correct grayscale data for the display hardware.
      const bool wasDarkMode = renderer.isDarkMode();
      renderer.setDarkMode(false);

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.copyGrayscaleLsbBuffers();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.copyGrayscaleMsbBuffers();

      // display grayscale part
      renderer.displayGrayBuffer();
      renderer.setRenderMode(GfxRenderer::BW);

      renderer.setDarkMode(wasDarkMode);
      renderer.restoreBwBuffer();
    } else {
      LOG_ERR("ERS", "Skipping grayscale pass; BW buffer store failed");
      decision.runGrayscalePass = false;
    }
    grayscaleMs = clampDurationMs(millis() - grayscaleStart);
  }

  ReaderRuntime::ReaderOperationTrace trace{};
  trace.readerKind = ReaderRuntime::ReaderKind::Epub;
  trace.operation = ReaderRuntime::ReaderOperation::RenderPage;
  trace.sectionIndex = static_cast<int16_t>(currentSpineIndex);
  trace.pageIndex = section ? static_cast<int16_t>(section->currentPage) : -1;
  trace.refreshMode = decision.mode;
  trace.refreshReason = decision.reason;
  trace.freeHeap = ESP.getFreeHeap();
  trace.minFreeHeap = ESP.getMinFreeHeap();
  trace.loadMs = loadMs;
  trace.preloadMs = preloadMs;
  trace.renderMs = renderMs;
  trace.displayMs = displayMs;
  trace.grayscaleMs = decision.runGrayscalePass ? grayscaleMs : 0;
  trace.totalMs = clampDurationMs(millis() - totalStart + loadMs);
  ReaderRuntime::setLastReaderOperationTrace(trace);

  if (trace.totalMs > 800) {
    LOG_DBG("ERS", "Slow page turn load=%ums preload=%ums render=%ums display=%ums gray=%ums total=%ums", trace.loadMs,
            trace.preloadMs, trace.renderMs, trace.displayMs, trace.grayscaleMs, trace.totalMs);
  }

  pendingStrongRefresh = false;
  lastSectionCacheRebuilt = false;
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string title;

  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    if (tocIndex == -1) {
      title = tr(STR_UNNAMED);
    } else {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  } else {
    title = "";
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title);
}

void EpubReaderActivity::invalidateSectionPreservingPosition() {
  RenderLock lock;
  if (section) {
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
    section.reset();
  }
  pendingStrongRefresh = true;
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  percent = clampPercent(percent);

  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  {
    RenderLock lock;
    if (currentSpineIndex != targetSpineIndex) {
      pendingStrongRefresh = true;
    }
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}
