#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // My Library, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsUrl) {
    count++;
  }
  return count;
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
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadNextRecentCover(int coverHeight) {
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
    if (StringUtils::checkFileExtension(book.path, ".epub")) {
      Epub epub(book.path, "/.crosspoint");
      if (epub.load(false, true)) {
        generated = epub.generateThumbBmp(coverHeight);
      }
    } else if (StringUtils::checkFileExtension(book.path, ".xtch") || StringUtils::checkFileExtension(book.path, ".xtc")) {
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
    requestUpdate();
    return;
  }

  recentsLoaded = true;
}
void HomeActivity::onEnter() {
  Activity::onEnter();

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;

  selectorIndex = 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_IDLE_MS;

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  const size_t bufferSize = renderer.getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  memcpy(coverBuffer, frameBuffer, bufferSize);
  coverBufferDarkMode = renderer.isDarkMode();
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) {
    return false;
  }
  if (coverBufferDarkMode != renderer.isDarkMode()) {
    freeCoverBuffer();
    coverRendered = false;
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  const size_t bufferSize = renderer.getBufferSize();
  memcpy(frameBuffer, coverBuffer, bufferSize);
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();

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

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Calculate dynamic indices based on which options are available
    int idx = 0;
    int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());
    const int myLibraryIdx = idx++;
    const int recentsIdx = idx++;
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex < recentBooks.size()) {
      activityManager.goToReader(recentBooks[selectorIndex].path);
    } else if (menuSelectedIndex == myLibraryIdx) {
      activityManager.goToFileBrowser();
    } else if (menuSelectedIndex == recentsIdx) {
      activityManager.goToRecentBooks();
    } else if (menuSelectedIndex == opdsLibraryIdx) {
      activityManager.goToBrowser();
    } else if (menuSelectedIndex == fileTransferIdx) {
      activityManager.goToFileTransfer();
    } else if (menuSelectedIndex == settingsIdx) {
      activityManager.goToSettings();
    }
    return;
  }

  if (firstRenderDone && !recentsLoaded && millis() >= nextRecentCoverLoadAt) {
    loadNextRecentCover(UITheme::getInstance().getMetrics().homeCoverHeight);
    nextRecentCoverLoadAt = millis() + RECENT_COVER_LOAD_INTERVAL_MS;
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsUrl) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  const auto hintInsets = GUI.getButtonHintInsets(renderer);
  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing;
  const int menuHeight = std::max(0, pageHeight - hintInsets.bottom - menuTop);
  const Rect menuRect{hintInsets.left, menuTop, pageWidth - hintInsets.left - hintInsets.right, menuHeight};

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  GUI.drawButtonMenu(
      renderer, menuRect, static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (renderer.isDarkMode()) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBuffer();
  }

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  }
}
