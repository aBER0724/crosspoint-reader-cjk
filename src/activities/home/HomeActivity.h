#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;
struct ThemeMetrics;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;        // Track if cover has been rendered once
  bool coverBufferStored = false;    // Track if cover buffer is stored
  bool coverBufferDarkMode = false;  // Reject cover snapshots created for the opposite display mode
  // First paint stays SD-I/O free so leaving a book reaches an interactive
  // home screen before cover decode/cropping begins.
  bool deferRecentCoverDraw = true;
  // When true, render() must repaint header/cover instead of the menu-only path.
  // Set on enter, cover regeneration, and any selection change that touches cover chrome.
  bool fullRedrawRequired = true;
  // Selector index from the last completed render; used to detect menu-only moves.
  int lastRenderedSelectorIndex = -1;
  bool backPressSeen = false;
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Defer cover generation so menu navigation stays responsive after thumb
  // cache version bumps force mass regeneration.
  static constexpr unsigned long RECENT_COVER_LOAD_IDLE_MS = 5000;
  static constexpr unsigned long RECENT_COVER_LOAD_INTERVAL_MS = 1000;
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  size_t nextRecentCoverIndex = 0;
  unsigned long nextRecentCoverLoadAt = 0;
  const HomeMenuItem initialMenuItem;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::RECENTS) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::RECENTS;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();

  int getMenuItemCount() const;
  // True when this selector index paints selection chrome on the recent-cover tile.
  // Themes with homeContinueReadingInMenu keep cover art static and put selection in the menu.
  bool isCoverSelectionIndex(int index) const;
  // Menu-only windowed refresh is safe when both the previously rendered and current
  // indices live purely in the button menu (no cover selection chrome changes).
  bool canUseMenuOnlyPartialUpdate(int fromIndex, int toIndex) const;
  // Cover-only windowed refresh requires the theme's unselected cover baseline.
  bool canUseCoverOnlyPartialUpdate(int fromIndex, int toIndex) const;
  int getHomeContentOffset(const ThemeMetrics& metrics) const;
  Rect getMenuRect(const ThemeMetrics& metrics, int pageWidth) const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadNextRecentCover(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
