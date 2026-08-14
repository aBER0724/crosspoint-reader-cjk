#include "TxtReaderActivity.h"

#include <Arduino.h>
#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderUtils.h"
#include "MappedInputManager.h"
#include "ReaderRuntimePolicy.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 4;          // Increment when cache format changes
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  activityManager.queueAppStateSave();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  pageStartsParagraph.clear();
  currentPageLines.clear();
  currentPageParagraphStarts.clear();
  saveProgress();
  APP_STATE.readerActivityLoadCount = 0;
  activityManager.queueAppStateSave();
  txt.reset();
}

void TxtReaderActivity::loop() {
  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  const bool inputActive = mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() ||
                           mappedInput.isPressed(MappedInputManager::Button::Back) ||
                           mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                           mappedInput.isPressed(MappedInputManager::Button::PageBack) ||
                           mappedInput.isPressed(MappedInputManager::Button::PageForward) ||
                           mappedInput.isPressed(MappedInputManager::Button::Power) || gpio.wasTouchActivity() ||
                           touch.prev || touch.next;
  if (inputActive) {
    if (!readerInputActive) {
      // Input actions run on the main task. Stop the older render at its next
      // safe boundary so Back/Home and the next page turn don't wait for it.
      activityManager.cancelCurrentRender();
      readerInputActive = true;
    }
  } else {
    readerInputActive = false;
  }

  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, txt ? txt->getPath().c_str() : "",
                                        {this, [](void* ctx) { static_cast<TxtReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    prioritizeNextReaderRender();
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      prioritizeNextReaderRender();
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

bool TxtReaderActivity::initializeReader(RenderLock& lock) {
  if (initialized) {
    return true;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  cachedFirstLineIndent = SETTINGS.firstLineIndent;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int baseLineHeight = renderer.getLineHeight(cachedFontId);
  const int lineHeight = std::max(1, static_cast<int>(baseLineHeight * SETTINGS.getReaderLineCompression() + 0.5f));

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  firstLineIndentWidth = 0;
  if (cachedFirstLineIndent) {
    const int ideographWidth = renderer.getTextAdvanceX(cachedFontId, "\xE4\xB8\x80", EpdFontFamily::REGULAR);
    const int requestedIndent =
        ideographWidth > 0 ? ideographWidth * 2 : renderer.getSpaceWidth(cachedFontId, EpdFontFamily::REGULAR) * 6;
    firstLineIndentWidth = std::min(std::max(0, viewportWidth - 1), std::max(0, requestedIndent));
  }

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    if (!buildPageIndex(lock)) {
      return false;
    }
    // Save to cache for next time
    savePageIndexCache();
  }

  if (lock.isStale()) {
    return false;
  }

  // Load saved progress
  loadProgress();

  if (lock.isStale()) {
    return false;
  }

  initialized = true;
  return true;
}

bool TxtReaderActivity::buildPageIndex(RenderLock& lock) {
  pageOffsets.clear();
  pageStartsParagraph.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0
  pageStartsParagraph.push_back(true);

  size_t offset = 0;
  bool offsetStartsParagraph = true;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));
  auto lastYieldMs = millis();

  while (offset < fileSize) {
    if (lock.isStale()) {
      pageOffsets.clear();
      pageStartsParagraph.clear();
      return false;
    }
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;
    bool nextOffsetStartsParagraph = offsetStartsParagraph;

    if (!loadPageAtOffset(offset, offsetStartsParagraph, tempLines, nullptr, nextOffset, nextOffsetStartsParagraph,
                          &lock)) {
      if (lock.isStale()) {
        pageOffsets.clear();
        pageStartsParagraph.clear();
        return false;
      }
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    offsetStartsParagraph = nextOffsetStartsParagraph;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
      pageStartsParagraph.push_back(offsetStartsParagraph);
    }

    // Keep the input loop scheduled while the uncached index is being built.
    // This is time based rather than page-count based because page layout cost
    // varies significantly by font and language.
    if (millis() - lastYieldMs >= 2) {
      vTaskDelay(1);
      lastYieldMs = millis();
    }
  }

  if (lock.isStale()) {
    pageOffsets.clear();
    pageStartsParagraph.clear();
    return false;
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
  return true;
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, bool offsetStartsParagraph, std::vector<std::string>& outLines,
                                         std::vector<uint8_t>* outParagraphStarts, size_t& nextOffset,
                                         bool& nextOffsetStartsParagraph, const RenderLock* lock) {
  outLines.clear();
  if (outParagraphStarts != nullptr) {
    outParagraphStarts->clear();
  }
  nextOffsetStartsParagraph = offsetStartsParagraph;
  const size_t fileSize = txt->getFileSize();
  const auto isStale = [lock]() { return lock != nullptr && lock->isStale(); };
  const auto clearOutput = [&outLines, outParagraphStarts]() {
    outLines.clear();
    if (outParagraphStarts != nullptr) {
      outParagraphStarts->clear();
    }
  };

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  if (isStale()) {
    free(buffer);
    return false;
  }

  // Prime the SD card font's advance table with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
    if (isStale()) {
      free(buffer);
      return false;
    }
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    if (isStale()) {
      clearOutput();
      free(buffer);
      return false;
    }
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      if ((lineEnd & 0x7F) == 0 && isStale()) {
        clearOutput();
        free(buffer);
        return false;
      }
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;
    bool firstVisualLine = offsetStartsParagraph;

    // Emit at least one visual line for each source line (including blank lines),
    // then continue with wrapping when needed.
    do {
      if (line.empty()) {
        outLines.emplace_back();
        if (outParagraphStarts != nullptr) {
          outParagraphStarts->push_back(false);
        }
        break;
      }

      int lineWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);
      const bool applyIndent = firstVisualLine && cachedParagraphAlignment != CrossPointSettings::CENTER_ALIGN;
      const int availableWidth = applyIndent ? viewportWidth - firstLineIndentWidth : viewportWidth;

      if (lineWidth <= availableWidth) {
        outLines.push_back(line);
        if (outParagraphStarts != nullptr) {
          outParagraphStarts->push_back(firstVisualLine);
        }
        lineBytePos = displayLen;  // Consumed entire display content
        line.clear();
        break;
      }

      // Find break point
      size_t breakPos = line.length();
      while (breakPos > 0 && renderer.getTextAdvanceX(cachedFontId, line.substr(0, breakPos).c_str(),
                                                      EpdFontFamily::REGULAR) > availableWidth) {
        if ((breakPos & 0x7F) == 0 && isStale()) {
          clearOutput();
          free(buffer);
          return false;
        }
        // Try to break at space
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          // Break at character boundary for UTF-8
          breakPos--;
          // Make sure we don't break in the middle of a UTF-8 sequence
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
            breakPos--;
          }
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      outLines.push_back(line.substr(0, breakPos));
      if (outParagraphStarts != nullptr) {
        outParagraphStarts->push_back(firstVisualLine);
      }
      firstVisualLine = false;

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
      offsetStartsParagraph = true;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      offsetStartsParagraph = false;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;
  nextOffsetStartsParagraph = offsetStartsParagraph;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  const bool hasLines = !outLines.empty() && !isStale();
  if (!hasLines) {
    clearOutput();
  }
  free(buffer);

  return hasLines;
}

void TxtReaderActivity::render(RenderLock&& lock) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized && !initializeReader(lock)) {
    return;
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    if (renderer.isDarkMode()) {
      renderer.displayBufferDarkRedrive();
    } else {
      renderer.displayBuffer();
    }
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  currentPageParagraphStarts.clear();
  bool nextOffsetStartsParagraph = false;
  const bool offsetStartsParagraph = currentPage < static_cast<int>(pageStartsParagraph.size())
                                         ? pageStartsParagraph[currentPage] != 0
                                         : currentPage == 0;
  loadPageAtOffset(offset, offsetStartsParagraph, currentPageLines, &currentPageParagraphStarts, nextOffset,
                   nextOffsetStartsParagraph, &lock);

  if (lock.isStale()) {
    return;
  }

  renderer.clearScreen();
  const uint32_t pendingInteractiveGeneration = interactiveRenderGeneration.load(std::memory_order_acquire);
  const bool interactiveRender = pendingInteractiveGeneration != 0 && pendingInteractiveGeneration <= lock.generation();
  renderPage(lock, interactiveRender);

  if (lock.isStale()) {
    return;
  }

  consumeInteractiveRender(lock);

  // Atomic FAT replacement runs only after the UI has been idle.
  saveProgress();
}

void TxtReaderActivity::renderPage(RenderLock& lock, const bool interactiveRender) {
  const int baseLineHeight = renderer.getLineHeight(cachedFontId);
  const int lineHeight = std::max(1, static_cast<int>(baseLineHeight * SETTINGS.getReaderLineCompression() + 0.5f));
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (size_t lineIndex = 0; lineIndex < currentPageLines.size(); ++lineIndex) {
      const auto& line = currentPageLines[lineIndex];
      if (lock.isStale()) {
        return false;
      }
      if (!line.empty()) {
        const int indent = lineIndex < currentPageParagraphStarts.size() && currentPageParagraphStarts[lineIndex] &&
                                   cachedParagraphAlignment != CrossPointSettings::CENTER_ALIGN
                               ? firstLineIndentWidth
                               : 0;
        const int lineWidthAvailable = contentWidth - indent;
        int x = cachedOrientedMarginLeft + indent;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + lineWidthAvailable - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
    return true;
  };

  // A scan/prewarm pass doubles the text work. Keep it for idle redraws but
  // draw immediately on a page turn or reader transition.
  if (!interactiveRender) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      auto scope = fcm->createPrewarmScope();
      if (!renderLines()) return;  // scan pass - text accumulated, no drawing
      scope.endScanAndPrewarm();
    }
  }

  // BW rendering
  if (!renderLines()) return;
  renderStatusBar();
  if (lock.isStale()) {
    return;
  }

  ReaderRuntime::RefreshContext refreshContext{};
  refreshContext.readerKind = ReaderRuntime::ReaderKind::Txt;
  refreshContext.darkMode = renderer.isDarkMode();
  refreshContext.textAntiAliasing = SETTINGS.textAntiAliasing;
  refreshContext.grayscaleRequested = !interactiveRender && SETTINGS.textAntiAliasing;
  refreshContext.lowMemory =
      ReaderRuntime::classifyReaderMemory(ESP.getFreeHeap()) != ReaderRuntime::MemoryDecision::Proceed;
  refreshContext.cadenceRemaining = pagesUntilFullRefresh;
  refreshContext.refreshFrequency = SETTINGS.getRefreshFrequency();

  const auto decision = ReaderRuntime::chooseReaderRefresh(refreshContext);
  const bool asyncTextRefresh = !renderer.isDarkMode() && !decision.runGrayscalePass && renderer.supportsAsyncRefresh();
  ReaderUtils::displayWithRefreshDecision(renderer, decision, asyncTextRefresh);
  pagesUntilFullRefresh = decision.nextCadenceRemaining;

  if (decision.runGrayscalePass) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { return renderLines(); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  int displayPage = currentPage + 1;
  int displayTotal = totalPages;
  if (displayPage < 0) displayPage = 0;
  if (displayTotal < 0) displayTotal = 0;
  if (displayTotal > 0 && displayPage > displayTotal) displayPage = displayTotal;
  if (displayTotal == 0 && displayPage > 0) displayTotal = displayPage;
  const float progress = displayTotal > 0 ? displayPage * 100.0f / displayTotal : 0;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, displayPage, displayTotal, title);
}

void TxtReaderActivity::saveProgress() const {
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = 0;
  data[3] = 0;
  if (txt) EpubReaderUtils::queueProgressSave(txt->getCachePath(), data, sizeof(data));
}

void TxtReaderActivity::prioritizeNextReaderRender() {
  interactiveRenderGeneration.store(activityManager.nextRenderGeneration(), std::memory_order_release);
}

void TxtReaderActivity::consumeInteractiveRender(const RenderLock& lock) {
  uint32_t expectedGeneration = interactiveRenderGeneration.load(std::memory_order_acquire);
  if (expectedGeneration != 0 && expectedGeneration <= lock.generation()) {
    interactiveRenderGeneration.compare_exchange_strong(expectedGeneration, 0, std::memory_order_release,
                                                        std::memory_order_relaxed);
  }
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - bool: first-line indent setting (to invalidate cache on layout change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets
  // - N * uint8_t: whether each page starts at a paragraph boundary

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  bool firstLineIndent;
  serialization::readPod(f, firstLineIndent);
  if (firstLineIndent != cachedFirstLineIndent) {
    LOG_DBG("TRS", "Cache first-line indent mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);
  pageStartsParagraph.clear();
  pageStartsParagraph.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  for (uint32_t i = 0; i < numPages; i++) {
    uint8_t startsParagraph;
    serialization::readPod(f, startsParagraph);
    pageStartsParagraph.push_back(startsParagraph != 0);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, cachedFirstLineIndent);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  for (uint8_t startsParagraph : pageStartsParagraph) {
    serialization::writePod(f, startsParagraph);
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
