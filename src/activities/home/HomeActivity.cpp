#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

bool HomeActivity::isCoverSelectionIndex(const int index) const {
  if (recentBooks.empty() || index < 0) {
    return false;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (metrics.homeContinueReadingInMenu) {
    // Cover tile is decorative; selection lives entirely in the button menu.
    return false;
  }

  // Base/Lyra paint selection chrome on recent tiles for indices in [0, recentCount).
  return index < static_cast<int>(recentBooks.size());
}

bool HomeActivity::canUseMenuOnlyPartialUpdate(const int fromIndex, const int toIndex) const {
  if (!firstRenderDone || fullRedrawRequired || fromIndex < 0 || fromIndex == toIndex) {
    return false;
  }
  return !isCoverSelectionIndex(fromIndex) && !isCoverSelectionIndex(toIndex);
}

bool HomeActivity::canUseCoverOnlyPartialUpdate(const int fromIndex, const int toIndex) const {
  if (!firstRenderDone || fullRedrawRequired || fromIndex < 0 || fromIndex == toIndex) {
    return false;
  }
  return isCoverSelectionIndex(fromIndex) && isCoverSelectionIndex(toIndex) && GUI.supportsHomeCoverSelectionUpdates();
}

Rect HomeActivity::getMenuRect(const ThemeMetrics& metrics, const int pageWidth, const int pageHeight) const {
  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int menuHeight =
      std::max(0, pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                                metrics.homeMenuTopOffset + metrics.buttonHintsHeight));
  return Rect{0, menuTop, pageWidth, menuHeight};
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadNextRecentCover(int coverHeight) {
  // The first frame intentionally uses an empty-cover placeholder. Once the
  // user has been idle, repaint existing thumbnails before any potentially
  // expensive cache generation below.
  if (deferRecentCoverDraw) {
    deferRecentCoverDraw = false;
    coverRendered = false;
    fullRedrawRequired = true;
    requestUpdate();
    return;
  }

  // Generate at most one missing cover per call so home navigation stays responsive
  // even after thumb_v2 cache misses force mass regeneration.
  while (nextRecentCoverIndex < recentBooks.size()) {
    RecentBook& book = recentBooks[nextRecentCoverIndex++];
    if (book.coverBmpPath.empty()) {
      continue;
    }

    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
    if (Storage.exists(coverPath.c_str())) {
      continue;
    }

    bool generated = false;
    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, "/.crosspoint");
      // Skip CSS; metadata/cover only.
      if (epub.load(false, true)) {
        generated = epub.generateThumbBmp(coverHeight);
      }
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, "/.crosspoint");
      if (xtc.load()) {
        generated = xtc.generateThumbBmp(coverHeight);
      }
    }

    if (!generated) {
      RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
      book.coverBmpPath = "";
    }

    coverRendered = false;
    fullRedrawRequired = true;
    requestUpdate();
    return;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  nextRecentCoverIndex = 0;
  recentsLoaded = recentBooks.empty();
  recentsLoading = false;
  nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_IDLE_MS;

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);
  lastRenderedSelectorIndex = -1;
  fullRedrawRequired = true;
  firstRenderDone = false;
  coverRendered = false;
  coverBufferStored = false;
  deferRecentCoverDraw = true;

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  coverBufferDarkMode = renderer.isDarkMode();
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  if (coverBufferDarkMode != renderer.isDarkMode()) {
    freeCoverBuffer();
    coverRendered = false;
    fullRedrawRequired = true;
    return false;
  }
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::RECENTS:
        onRecentsOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_IDLE_MS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_IDLE_MS;
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_IDLE_MS;
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_IDLE_MS;
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card). backPressSeen guards against the stale
  // release of the Back press that closed the previous activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backPressSeen && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  int tx = 0;
  int ty = 0;
  if (!recentBooks.empty() && mappedInput.wasScreenTouchDown(tx, ty) && tx >= 0 && tx < renderer.getScreenWidth() &&
      ty >= metrics.homeTopPadding && ty < metrics.homeTopPadding + metrics.homeCoverTileHeight) {
    if (selectorIndex != 0) {
      selectorIndex = 0;
      nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_IDLE_MS;
      requestUpdate();
    }
    return;
  }

  if (!recentBooks.empty() &&
      mappedInput.wasTapInRect(0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight)) {
    selectorIndex = 0;
    activateSelection();
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, metrics.menuRowHeight + metrics.menuSpacing,
                                              renderedMenuCount, 0, INT32_MAX, metrics.menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_IDLE_MS;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
    return;
  }

  if (firstRenderDone && !recentsLoaded && !recentsLoading && millis() >= nextRecentCoverLoadAt) {
    recentsLoading = true;
    loadNextRecentCover(metrics.homeCoverHeight);
    recentsLoading = false;
    nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_INTERVAL_MS;
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect menuRect = getMenuRect(metrics, pageWidth, pageHeight);
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  const unsigned long renderStart = millis();
  unsigned long afterClear = renderStart;
  unsigned long afterRestore = renderStart;
  unsigned long afterHeader = renderStart;
  unsigned long afterCover = renderStart;
  unsigned long afterMenu = renderStart;
#endif

  std::array<const char*, 6> menuItems{};
  std::array<UIIcon, 6> menuIcons{};
  int menuItemCount = 0;
  const auto addMenuItem = [&](const char* label, UIIcon icon) {
    menuItems[menuItemCount] = label;
    menuIcons[menuItemCount++] = icon;
  };
  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    addMenuItem(tr(STR_CONTINUE_READING), Book);
  }
  addMenuItem(tr(STR_BROWSE_FILES), Folder);
  addMenuItem(tr(STR_MENU_RECENT_BOOKS), Recent);
  if (hasOpdsServers) {
    addMenuItem(tr(STR_OPDS_BROWSER), Library);
  }
  addMenuItem(tr(STR_FILE_TRANSFER), Transfer);
  addMenuItem(tr(STR_SETTINGS_TITLE), Settings);

  const bool menuOnlyPartialUpdate = canUseMenuOnlyPartialUpdate(lastRenderedSelectorIndex, selectorIndex);
  bool coverOnlyPartialUpdate = canUseCoverOnlyPartialUpdate(lastRenderedSelectorIndex, selectorIndex);
  const int selectedMenuIndex =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - static_cast<int>(recentBooks.size());
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  LOG_INF("HOME", "menu-update from=%d to=%d recent=%u first=%d full=%d cover=%d/%d partial=%d rows=%d",
          lastRenderedSelectorIndex, selectorIndex, static_cast<unsigned>(recentBooks.size()), firstRenderDone ? 1 : 0,
          fullRedrawRequired ? 1 : 0, isCoverSelectionIndex(lastRenderedSelectorIndex) ? 1 : 0,
          isCoverSelectionIndex(selectorIndex) ? 1 : 0, menuOnlyPartialUpdate ? 1 : 0,
          GUI.supportsHomeMenuRowUpdates() ? 1 : 0);
#endif
  int firstUpdatedMenuIndex = 0;
  int lastUpdatedMenuIndex = menuItemCount - 1;
  Rect coverDirtyRect;
  bool bufferRestored = false;

  if (coverOnlyPartialUpdate) {
    bufferRestored = coverBufferStored && restoreCoverBuffer();
    coverOnlyPartialUpdate = bufferRestored;
    if (coverOnlyPartialUpdate) {
      coverDirtyRect = GUI.drawHomeCoverSelectionUpdate(
          renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, recentBooks,
          lastRenderedSelectorIndex, selectorIndex);
      coverOnlyPartialUpdate = coverDirtyRect.width > 0 && coverDirtyRect.height > 0;
      if (coverOnlyPartialUpdate) {
        renderer.setPartialUpdateRect(coverDirtyRect.x, coverDirtyRect.y, coverDirtyRect.width, coverDirtyRect.height);
      }
    }
  }

  if (menuOnlyPartialUpdate) {
    const int previousMenuIndex = metrics.homeContinueReadingInMenu
                                      ? lastRenderedSelectorIndex
                                      : lastRenderedSelectorIndex - static_cast<int>(recentBooks.size());
    const int currentMenuIndex =
        metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - static_cast<int>(recentBooks.size());
    firstUpdatedMenuIndex = std::min(previousMenuIndex, currentMenuIndex);
    lastUpdatedMenuIndex = std::max(previousMenuIndex, currentMenuIndex);
    const Rect dirtyRect = GUI.getHomeMenuDirtyRect(menuRect, previousMenuIndex, currentMenuIndex);
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
    LOG_INF("HOME", "menu-window rows=%d..%d logical=%d,%d %dx%d", firstUpdatedMenuIndex, lastUpdatedMenuIndex,
            dirtyRect.x, dirtyRect.y, dirtyRect.width, dirtyRect.height);
#endif
    // Clear and rebuild exactly the menu rows whose selected state changed.
    renderer.fillRect(dirtyRect.x, dirtyRect.y, dirtyRect.width, dirtyRect.height, false);
    renderer.setPartialUpdateRect(dirtyRect.x, dirtyRect.y, dirtyRect.width, dirtyRect.height);
  } else if (!coverOnlyPartialUpdate) {
    renderer.clearScreen();
  }
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  afterClear = millis();
#endif

  if (!menuOnlyPartialUpdate && !coverOnlyPartialUpdate) {
    bufferRestored = coverBufferStored && restoreCoverBuffer();
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
    afterRestore = millis();
#endif

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                   metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
    afterHeader = millis();
#endif

    // Record the tile rect so storeCoverBuffer (called from the theme) knows
    // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
    // instead of the 48 KB full framebuffer the previous bind captured.
    coverRectX = 0;
    coverRectY = metrics.homeTopPadding;
    coverRectW = pageWidth;
    coverRectH = metrics.homeCoverTileHeight;

    // Preserve the actual book title and selection layout, but suppress the
    // first frame's synchronous bitmap read/crop. The idle cover job below
    // invalidates this snapshot and draws the real thumbnail later.
    std::vector<RecentBook> coverBooks;
    const std::vector<RecentBook>* booksForCover = &recentBooks;
    if (deferRecentCoverDraw) {
      coverBooks = recentBooks;
      for (RecentBook& book : coverBooks) {
        book.coverBmpPath.clear();
      }
      booksForCover = &coverBooks;
    }
    GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                            *booksForCover, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                            std::bind(&HomeActivity::storeCoverBuffer, this));
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
    afterCover = millis();
#endif
  }

  const auto menuLabel = [&menuItems](int index) { return menuItems[static_cast<size_t>(index)]; };
  const auto menuIcon = [&menuIcons](int index) { return menuIcons[static_cast<size_t>(index)]; };
  if (menuOnlyPartialUpdate && GUI.supportsHomeMenuRowUpdates()) {
    GUI.drawButtonMenuRange(renderer, menuRect, menuItemCount, selectedMenuIndex, menuLabel, menuIcon,
                            firstUpdatedMenuIndex, lastUpdatedMenuIndex);
  } else if (!coverOnlyPartialUpdate) {
    GUI.drawButtonMenu(renderer, menuRect, menuItemCount, selectedMenuIndex, menuLabel, menuIcon);
  }
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  afterMenu = millis();
#endif

  if (!menuOnlyPartialUpdate && !coverOnlyPartialUpdate) {
    const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                              tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  if (!menuOnlyPartialUpdate && !coverOnlyPartialUpdate &&
      (isCoverSelectionIndex(lastRenderedSelectorIndex) || isCoverSelectionIndex(selectorIndex))) {
    const unsigned long afterHints = millis();
    LOG_INF("HOME", "cover-profile clear=%lu restore=%lu header=%lu cover=%lu menu=%lu hints=%lu total=%lu",
            afterClear - renderStart, afterRestore - afterClear, afterHeader - afterRestore, afterCover - afterHeader,
            afterMenu - afterCover, afterHints - afterMenu, afterHints - renderStart);
  }
  if (coverOnlyPartialUpdate) {
    LOG_INF("HOME", "cover-window logical=%d,%d %dx%d cpu=%lu", coverDirtyRect.x, coverDirtyRect.y,
            coverDirtyRect.width, coverDirtyRect.height, millis() - renderStart);
  }
#endif

  // Menu-only path uses setPartialUpdateRect + displayBuffer so dark mode still
  // takes the windowed dark redrive branch. Full redraws keep the existing policy.
  if (menuOnlyPartialUpdate || coverOnlyPartialUpdate) {
    renderer.displayBuffer();
  } else if (renderer.isDarkMode()) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBufferAsync();
  }

  lastRenderedSelectorIndex = selectorIndex;
  fullRedrawRequired = false;

  if (!firstRenderDone) {
    firstRenderDone = true;
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
