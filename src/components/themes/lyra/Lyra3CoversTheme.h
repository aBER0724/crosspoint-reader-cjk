

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

namespace Lyra3CoversMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeCoverTileHeight = 300;
  v.homeRecentBooksCount = 3;
  return v;
}();
}  // namespace Lyra3CoversMetrics

class Lyra3CoversTheme : public LyraTheme {
  mutable uint8_t* selectionBuffer = nullptr;
  mutable size_t selectionBufferSize = 0;
  mutable size_t selectionTileBufferSize = 0;
  mutable int selectionBufferCount = 0;
  mutable Rect selectionBufferRect;
  mutable bool selectionBuffersReady = false;
  mutable bool selectionBuffersUnavailable = false;

  void resetSelectionBuffers() const;
  bool buildSelectionBuffers(GfxRenderer& renderer, Rect coverRect, const std::vector<RecentBook>& recentBooks) const;
  bool restoreSelectionBuffer(GfxRenderer& renderer, int selectorIndex) const;

 public:
  ~Lyra3CoversTheme() override { resetSelectionBuffers(); }
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer,
                           const std::function<bool()>& isCancelled) const override;
  bool supportsHomeCoverSelectionUpdates() const override { return true; }
  Rect drawHomeCoverSelectionUpdate(GfxRenderer& renderer, Rect coverRect, const std::vector<RecentBook>& recentBooks,
                                    int previousSelectorIndex, int selectorIndex) const override;
};
