#include "Lyra3CoversTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;

int coverCount(const std::vector<RecentBook>& recentBooks) {
  return std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount);
}

struct CoverTitleLayout {
  std::vector<std::string> lines;
  int lineHeight;
  int selectionHeight;
};

CoverTitleLayout getCoverTitleLayout(const GfxRenderer& renderer, const RecentBook& book, int tileWidth) {
  CoverTitleLayout layout{
      renderer.wrappedText(SMALL_FONT_ID, book.title.c_str(), tileWidth - 2 * hPaddingInSelection, 3),
      renderer.getLineHeight(SMALL_FONT_ID), 0};
  layout.selectionHeight = static_cast<int>(layout.lines.size()) * layout.lineHeight + hPaddingInSelection + 5;
  return layout;
}

void drawCoverTitle(GfxRenderer& renderer, int tileX, int tileY, const CoverTitleLayout& layout) {
  int currentY = tileY + Lyra3CoversMetrics::values.homeCoverHeight + hPaddingInSelection + 5;
  for (const auto& line : layout.lines) {
    renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
    currentY += layout.lineHeight;
  }
}

void drawSelection(GfxRenderer& renderer, int tileX, int tileY, int tileWidth, int titleHeight) {
  renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                           Color::LightGray);
  renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                          Lyra3CoversMetrics::values.homeCoverHeight, Color::LightGray);
  renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection, hPaddingInSelection,
                          Lyra3CoversMetrics::values.homeCoverHeight, Color::LightGray);
  renderer.fillRoundedRect(tileX, tileY + Lyra3CoversMetrics::values.homeCoverHeight + hPaddingInSelection, tileWidth,
                           titleHeight, cornerRadius, false, false, true, true, Color::LightGray);
}
}  // namespace

void Lyra3CoversTheme::resetSelectionBuffers() const {
  if (selectionBuffer) {
    free(selectionBuffer);
    selectionBuffer = nullptr;
  }
  selectionBufferSize = 0;
  selectionTileBufferSize = 0;
  selectionBufferCount = 0;
  selectionBufferRect = Rect{};
  selectionBuffersReady = false;
  selectionBuffersUnavailable = false;
}

bool Lyra3CoversTheme::buildSelectionBuffers([[maybe_unused]] GfxRenderer& renderer,
                                             [[maybe_unused]] const Rect coverRect,
                                             [[maybe_unused]] const std::vector<RecentBook>& recentBooks) const {
  // Home already keeps one compact baseline for the complete cover strip.
  // Caching another selected copy of every tile consumes the contiguous heap
  // Reader needs for its parser, fonts, and page buffers.
  resetSelectionBuffers();
  selectionBuffersUnavailable = true;
  return false;
}
bool Lyra3CoversTheme::restoreSelectionBuffer(GfxRenderer& renderer, const int selectorIndex) const {
  if (!selectionBuffersReady || !selectionBuffer || selectorIndex < 0 || selectorIndex >= selectionBufferCount) {
    return false;
  }

  const int tileX = selectionBufferRect.x + selectorIndex * selectionBufferRect.width;
  const bool restored =
      renderer.copyBufferToRegion(tileX, selectionBufferRect.y, selectionBufferRect.width, selectionBufferRect.height,
                                  selectionBuffer + selectorIndex * selectionTileBufferSize, selectionTileBufferSize);
#if defined(SSD1677_PROBE_DEBUG) && SSD1677_PROBE_DEBUG
  LOG_INF("HOME", "cover-cache select=%d hit=%d", selectorIndex, restored ? 1 : 0);
#endif
  return restored;
}

void Lyra3CoversTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer,
                                           const std::function<bool()>& isCancelled) const {
  if (isCancelled && isCancelled()) return;
  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  const int tileY = rect.y;
  const int recentCount = coverCount(recentBooks);
  const bool hasContinueReading = recentCount > 0;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      resetSelectionBuffers();
      for (int i = 0; i < recentCount; i++) {
        if (isCancelled && isCancelled()) return;

        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath =
              UITheme::getCoverThumbPath(coverPath, Lyra3CoversMetrics::values.homeCoverHeight);

          // First time: load cover from SD and render
          FsFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              float coverHeight = static_cast<float>(bitmap.getHeight());
              float coverWidth = static_cast<float>(bitmap.getWidth());
              float ratio = coverWidth / coverHeight;
              const float tileRatio = static_cast<float>(tileWidth - 2 * hPaddingInSelection) /
                                      static_cast<float>(Lyra3CoversMetrics::values.homeCoverHeight);
              float cropX = 1.0f - (tileRatio / ratio);

              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  tileWidth - 2 * hPaddingInSelection, Lyra3CoversMetrics::values.homeCoverHeight,
                                  cropX, 0, isCancelled);

            } else {
              hasCover = false;
            }
            file.close();
          }
        }
        // Draw either way
        renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                          Lyra3CoversMetrics::values.homeCoverHeight, true);

        if (!hasCover) {
          // Render empty cover
          renderer.fillRect(tileX + hPaddingInSelection,
                            tileY + hPaddingInSelection + (Lyra3CoversMetrics::values.homeCoverHeight / 3),
                            tileWidth - 2 * hPaddingInSelection, 2 * Lyra3CoversMetrics::values.homeCoverHeight / 3,
                            true);
          renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32);
        }
      }

      // Store an unselected baseline so focus moves only need to restore it
      // and redraw the new selection chrome.
      for (int i = 0; i < recentCount; i++) {
        if (isCancelled && isCancelled()) return;
        const int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
        const auto title = getCoverTitleLayout(renderer, recentBooks[i], tileWidth);
        drawCoverTitle(renderer, tileX, tileY, title);
      }
      coverBufferStored = storeCoverBuffer();

      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    if (selectorIndex >= 0 && selectorIndex < recentCount) {
      if (isCancelled && isCancelled()) return;
      if (bufferRestored && !selectionBuffersReady && !selectionBuffersUnavailable) {
        buildSelectionBuffers(renderer, rect, recentBooks);
      }
      if (!restoreSelectionBuffer(renderer, selectorIndex)) {
        const int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * selectorIndex;
        const auto title = getCoverTitleLayout(renderer, recentBooks[selectorIndex], tileWidth);
        drawSelection(renderer, tileX, tileY, tileWidth, title.selectionHeight);
        drawCoverTitle(renderer, tileX, tileY, title);
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

Rect Lyra3CoversTheme::drawHomeCoverSelectionUpdate(GfxRenderer& renderer, const Rect rect,
                                                    const std::vector<RecentBook>& recentBooks,
                                                    const int previousSelectorIndex, const int selectorIndex) const {
  const int recentCount = coverCount(recentBooks);
  if (previousSelectorIndex < 0 || previousSelectorIndex >= recentCount || selectorIndex < 0 ||
      selectorIndex >= recentCount) {
    return Rect{};
  }

  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  if (!restoreSelectionBuffer(renderer, selectorIndex)) {
    const int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * selectorIndex;
    const auto title = getCoverTitleLayout(renderer, recentBooks[selectorIndex], tileWidth);
    drawSelection(renderer, tileX, rect.y, tileWidth, title.selectionHeight);
    drawCoverTitle(renderer, tileX, rect.y, title);
  }

  const int firstTile = std::min(previousSelectorIndex, selectorIndex);
  const int lastTile = std::max(previousSelectorIndex, selectorIndex);
  return Rect{Lyra3CoversMetrics::values.contentSidePadding + firstTile * tileWidth, rect.y,
              (lastTile - firstTile + 1) * tileWidth, rect.height};
}
