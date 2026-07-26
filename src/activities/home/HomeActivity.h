#pragma once
#include <functional>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsUrl = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  bool coverBufferDarkMode = false;
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  static constexpr unsigned long RECENT_COVER_LOAD_IDLE_MS = 5000;
  static constexpr unsigned long RECENT_COVER_LOAD_INTERVAL_MS = 1000;

  std::vector<RecentBook> recentBooks;
  size_t nextRecentCoverIndex = 0;
  unsigned long nextRecentCoverLoadAt = 0;

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadNextRecentCover(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Home", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
