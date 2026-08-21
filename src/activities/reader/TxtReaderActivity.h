#pragma once

#include <Txt.h>

#include <atomic>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<uint8_t> pageStartsParagraph;
  std::vector<std::string> currentPageLines;
  std::vector<uint8_t> currentPageParagraphStarts;
  int linesPerPage = 0;
  int viewportWidth = 0;
  int firstLineIndentWidth = 0;
  bool initialized = false;
  bool readerInputActive = false;
  std::atomic<uint32_t> interactiveRenderGeneration{0};

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  bool cachedFirstLineIndent = false;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage(RenderLock& lock, bool interactiveRender);
  void renderStatusBar() const;
  void prioritizeNextReaderRender();
  void consumeInteractiveRender(const RenderLock& lock);

  bool initializeReader(RenderLock& lock);
  bool loadPageAtOffset(size_t offset, bool offsetStartsParagraph, std::vector<std::string>& outLines,
                        std::vector<uint8_t>* outParagraphStarts, size_t& nextOffset, bool& nextOffsetStartsParagraph,
                        const RenderLock* lock = nullptr);
  bool buildPageIndex(RenderLock& lock);
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : Activity("TxtReader", renderer, mappedInput), txt(std::move(txt)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool supportsLandscape() const override { return true; }
  bool isReaderActivity() const override { return true; }
  bool needsReaderFontMemory() const override { return true; }
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      pagesUntilFullRefresh = 1;
    }
    requestUpdate();
    return true;
  }
  ScreenshotInfo getScreenshotInfo() const override;
};
