#include "OtaUpdateActivity.h"

#include <FontCacheManager.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstring>
#include <new>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"

namespace {
// WiFi STA startup can wedge the calling task indefinitely on X4 (observed when
// the device is powered from a weak USB supply: the RF/BBPLL bring-up never
// returns and the calling task is lost while the scheduler keeps running). Run
// it on a short-lived worker and bound the wait so the flow fails gracefully
// instead of freezing the screen.
constexpr TickType_t WIFI_MODE_TIMEOUT_TICKS = pdMS_TO_TICKS(20000);

struct WifiModeRequest {
  SemaphoreHandle_t done;
  bool result;
};

void wifiModeWorker(void* ctx) {
  auto* request = static_cast<WifiModeRequest*>(ctx);
  request->result = WiFi.mode(WIFI_STA);
  xSemaphoreGive(request->done);
  vTaskDelete(nullptr);
}

bool wifiModeWithTimeout() {
  auto* request = new (std::nothrow) WifiModeRequest{};
  if (request == nullptr) {
    // No heap for the fallback path: call inline like the old code.
    return WiFi.mode(WIFI_STA);
  }
  request->done = xSemaphoreCreateBinary();
  if (request->done == nullptr) {
    delete request;
    return WiFi.mode(WIFI_STA);
  }
  TaskHandle_t worker = nullptr;
  if (xTaskCreate(wifiModeWorker, "ota_wifi_mode", 4096, request, 5, &worker) != pdPASS) {
    vSemaphoreDelete(request->done);
    delete request;
    return WiFi.mode(WIFI_STA);
  }
  const bool completed = xSemaphoreTake(request->done, WIFI_MODE_TIMEOUT_TICKS) == pdTRUE;
  if (!completed) {
    LOG_ERR("OTA", "WiFi.mode did not return within 20s; abandoning the startup task");
    // The worker may still be wedged inside the driver and will signal `done`
    // (then delete its task) if it ever unwedges. Ownership of the request and
    // the semaphore transfers to the worker, so the waiter must not free them.
    // The leak is bounded to one small allocation per abandoned attempt.
    return false;
  }
  // The worker deleted itself after give(); reclaim its context.
  vSemaphoreDelete(request->done);
  const bool result = request->result;
  delete request;
  return result;
}
}  // namespace

namespace {
struct OtaActionRects {
  Rect cancel;
  Rect update;
};

OtaActionRects getOtaActionRects(const GfxRenderer& renderer) {
  const int top = renderer.getScreenHeight() - 80;
  const int width = renderer.getScreenWidth() / 2;
  return {Rect{0, top, width, 80}, Rect{width, top, renderer.getScreenWidth() - width, 80}};
}

bool contains(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}
}  // namespace

bool OtaUpdateActivity::autoInstallPending = false;

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    finish();
    return;
  }
  OtaUpdater::recordInstallProgressForDiagnostics(0, 0);
  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock;
    state = CHECKING_FOR_UPDATE;
  }
  requestUpdateAndWait();

  // Same pre-TLS contract as FontDownloadActivity::beginNetworkTransfer:
  // release built-in glyph caches, resident SD fonts and the decompressor
  // cache before the TLS-heavy release-metadata fetch, because the WiFi
  // selector re-grew caches while it rendered.
  {
    RenderLock lock;
    LOG_DBG("OTA", "Heap before update-check cache release: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    FontManager::getInstance().releaseGlyphCaches();
    sdFontSystem.releaseResidentFonts(renderer);
    if (auto* cache = renderer.getFontCacheManager()) {
      cache->clearCache();
    }
  }
  LOG_DBG("OTA", "Heap before update check: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    {
      RenderLock lock;
      state = FAILED;
    }
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    {
      RenderLock lock;
      state = NO_UPDATE;
    }
    return;
  }

  {
    RenderLock lock;
    state = WAITING_CONFIRMATION;
  }
  if (autoInstallPending) {
    autoInstallPending = false;
    OtaUpdater::recordInstallProgressForDiagnostics(1, 0);
    runUpdateInstall();
  }
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // The CJK UI glyph caches (built-in font page buffers, the decompressor cache and
  // any resident SD faces) can hold most of the free heap, and WiFi STA startup
  // allocates its driver state in one burst. Releasing after WiFi.mode() is too
  // late on X4: WiFi init can fail with a null internal handle, or exhaust the
  // heap mid-init so libstdc++ throws std::bad_alloc with no handler and the
  // firmware abort()s (field crash: OTA check with ~54 KB free sampled, heap
  // actually bottoming out during wifi task/driver allocation). Same contract
  // as FontDownloadActivity::onEnter.
  {
    RenderLock lock(*this);
    LOG_DBG("OTA", "Heap before WiFi cache release: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    FontManager::getInstance().releaseGlyphCaches();
    sdFontSystem.releaseResidentFonts(renderer);
    if (auto* cache = renderer.getFontCacheManager()) {
      cache->clearCache();
    }
    LOG_DBG("OTA", "Heap before WiFi startup: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  // Turn on WiFi immediately
  // stage 8 = entered onEnter and about to call WiFi.mode; if the next boot
  // reports stage 8 with nothing later, the wedge is in WiFi STA startup.
  OtaUpdater::recordInstallProgressForDiagnostics(8, 0);
  LOG_DBG("OTA", "Turning on WiFi...");
  if (!wifiModeWithTimeout()) {
    OtaUpdater::recordInstallProgressForDiagnostics(9, 0);  // stage 9 = WiFi.mode returned false
    LOG_ERR("OTA", "Failed to initialize WiFi for update check");
    {
      RenderLock lock;
      state = FAILED;
    }
    requestUpdate();
    return;
  }
  OtaUpdater::recordInstallProgressForDiagnostics(7, 0);  // stage 7 = WiFi.mode returned true

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Turn off wifi
  WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  delay(100);              // Allow disconnect frame to be sent
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down

  // WiFi is fully powered down, so reloading the configured UI face is safe
  // again. Without this the device keeps rendering with built-in CJK glyphs
  // until the next font reload (user-visible "UI font reset to default").
  sdFontSystem.ensureUiLoaded(renderer);
}

bool OtaUpdateActivity::buildOtaProgressGlyphAtlas() {
  static constexpr char GLYPHS[] = "0123456789%/";

  otaProgressGlyphAtlasReady = false;
  ExternalFont* font = FontManager::getInstance().getActiveUiFont();
  if (!font || !font->isLoaded()) {
    return false;
  }

  for (int i = 0; i < OTA_PROGRESS_GLYPH_COUNT; ++i) {
    const char ch = GLYPHS[i];
    const auto* bitmap = font->getGlyph(static_cast<uint32_t>(ch));
    ExternalGlyphMetrics metrics{};
    if (!bitmap || !font->getGlyphMetrics(static_cast<uint32_t>(ch), &metrics)) {
      otaProgressGlyphAtlasReady = false;
      return false;
    }

    const uint8_t width = metrics.width > 0 ? metrics.width : font->getCharWidth();
    const uint8_t height = metrics.height > 0 ? metrics.height : font->getCharHeight();
    const uint8_t bytesPerRow = static_cast<uint8_t>((width + 7U) / 8U);
    const uint16_t byteCount = static_cast<uint16_t>(bytesPerRow) * height;
    if (byteCount > OTA_PROGRESS_GLYPH_MAX_BYTES) {
      otaProgressGlyphAtlasReady = false;
      return false;
    }

    OtaProgressGlyph& glyph = otaProgressGlyphs[i];
    glyph.ch = ch;
    glyph.width = width;
    glyph.height = height;
    glyph.bytesPerRow = bytesPerRow;
    glyph.advance = static_cast<uint8_t>(metrics.advanceX > 0 ? metrics.advanceX : width);
    std::memset(glyph.bitmap, 0, sizeof(glyph.bitmap));
    std::memcpy(glyph.bitmap, bitmap, byteCount);
  }

  otaProgressGlyphAtlasReady = true;
  return true;
}

const OtaUpdateActivity::OtaProgressGlyph* OtaUpdateActivity::findOtaProgressGlyph(char ch) const {
  for (int i = 0; i < OTA_PROGRESS_GLYPH_COUNT; ++i) {
    if (otaProgressGlyphs[i].ch == ch) {
      return &otaProgressGlyphs[i];
    }
  }
  return nullptr;
}

void OtaUpdateActivity::drawOtaProgressText(const char* text, int y) {
  if (!otaProgressGlyphAtlasReady || !text || *text == '\0') {
    return;
  }

  static constexpr int GLYPH_GAP = 2;
  int textWidth = 0;
  uint8_t lineHeight = 0;
  for (int i = 0; text[i] != '\0'; ++i) {
    if (text[i] == ' ') {
      textWidth += 8;
      continue;
    }
    const OtaProgressGlyph* glyph = findOtaProgressGlyph(text[i]);
    if (!glyph) {
      return;
    }
    textWidth += glyph->advance + GLYPH_GAP;
    if (glyph->height > lineHeight) {
      lineHeight = glyph->height;
    }
  }
  if (textWidth > 0) {
    textWidth -= GLYPH_GAP;
  }

  const int pageWidth = renderer.getScreenWidth();
  int x = (pageWidth - textWidth) / 2;
  if (x < 0) {
    x = 0;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.fillRect(0, y, pageWidth, lineHeight + metrics.verticalSpacing, false);
  for (int i = 0; text[i] != '\0'; ++i) {
    if (text[i] == ' ') {
      x += 8;
      continue;
    }

    const OtaProgressGlyph* glyph = findOtaProgressGlyph(text[i]);
    if (!glyph) {
      return;
    }

    for (uint8_t row = 0; row < glyph->height; ++row) {
      for (uint8_t col = 0; col < glyph->width; ++col) {
        const uint8_t byte = glyph->bitmap[row * glyph->bytesPerRow + (col >> 3)];
        const uint8_t bit = static_cast<uint8_t>(7U - (col & 0x07U));
        if ((byte >> bit) & 0x01U) {
          renderer.drawPixel(x + col, y + row);
        }
      }
    }
    x += glyph->advance + GLYPH_GAP;
  }
}

void OtaUpdateActivity::renderOtaProgressOnly(unsigned int percentage, size_t processedSize, size_t totalSize) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const int progressBarWidth = pageWidth - metrics.contentSidePadding * 2;
  const int progressBarY = (pageHeight - metrics.progressBarHeight) / 2;
  const Rect progressBarRect{metrics.contentSidePadding, progressBarY, progressBarWidth, metrics.progressBarHeight};

  // OTA leaves only a few KB of contiguous heap after esp_https_ota_begin().
  // Keep this low-memory path independent of the font system: static localized
  // text and progress glyphs are rendered/copied before OTA begins. During OTA
  // we only update primitive pixels and reuse the copied glyph bitmaps.
  renderer.fillRect(progressBarRect.x, progressBarRect.y, progressBarRect.width, progressBarRect.height, false);
  renderer.drawRect(progressBarRect.x, progressBarRect.y, progressBarRect.width, progressBarRect.height);
  const int fillWidth = (progressBarRect.width - 4) * static_cast<int>(percentage) / 100;
  if (fillWidth > 0) {
    renderer.fillRect(progressBarRect.x + 2, progressBarRect.y + 2, fillWidth, progressBarRect.height - 4);
  }

  char percentText[8];
  snprintf(percentText, sizeof(percentText), "%u%%", percentage);
  const int percentY = progressBarRect.y + progressBarRect.height + metrics.verticalSpacing;
  drawOtaProgressText(percentText, percentY);

  char bytesText[32];
  snprintf(bytesText, sizeof(bytesText), "%u / %u", static_cast<unsigned>(processedSize),
           static_cast<unsigned>(totalSize));
  const int bytesY = percentY + 20 + metrics.verticalSpacing;
  drawOtaProgressText(bytesText, bytesY);
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == UPDATE_IN_PROGRESS) {
    const auto processedSize = updater.getProcessedSize();
    const auto totalSize = updater.getTotalSize();
    LOG_DBG("OTA", "Update progress: %d / %d", processedSize, totalSize);

    if (totalSize == 0) {
      return;
    }

    const unsigned int updaterPercentage =
        static_cast<unsigned int>((static_cast<uint64_t>(processedSize) * 100U) / static_cast<uint64_t>(totalSize));

    if (lowMemoryOtaProgress) {
      if (updaterPercentage / 2U == lastUpdaterPercentage / 2U) {
        return;
      }
      lastUpdaterPercentage = updaterPercentage;
      renderOtaProgressOnly(updaterPercentage, processedSize, totalSize);
      if (renderer.isDarkMode()) {
        renderer.displayBufferDarkRedrive();
      } else {
        renderer.displayBuffer();
      }
      return;
    }
  }

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UPDATE));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NEW_UPDATE), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height + metrics.verticalSpacing,
                      (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height * 2 + metrics.verticalSpacing * 2,
                      (std::string(tr(STR_NEW_VERSION)) + updater.getLatestVersion()).c_str());

    const auto actionRects = getOtaActionRects(renderer);
    const int cancelTextWidth = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_CANCEL));
    renderer.drawText(UI_10_FONT_ID, actionRects.cancel.x + (actionRects.cancel.width - cancelTextWidth) / 2,
                      actionRects.cancel.y + 28, tr(STR_CANCEL));
    const int updateTextWidth = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_UPDATE));
    renderer.drawText(UI_10_FONT_ID, actionRects.update.x + (actionRects.update.width - updateTextWidth) / 2,
                      actionRects.update.y + 28, tr(STR_UPDATE));

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == UPDATE_IN_PROGRESS) {
    const int progressBarY = (pageHeight - metrics.progressBarHeight) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, progressBarY - height - metrics.verticalSpacing, tr(STR_UPDATING));
    renderOtaProgressOnly(0, updater.getProcessedSize(), updater.getTotalSize());
    renderer.drawCenteredText(UI_10_FONT_ID,
                              progressBarY + metrics.progressBarHeight + height * 2 + metrics.verticalSpacing * 3,
                              tr(STR_SD_FIRMWARE_DO_NOT_POWER_OFF), true);
  } else if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_UPDATE), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == REBOOTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_REBOOTING_TO_INSTALL), true, EpdFontFamily::BOLD);
  } else if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, tr(STR_POWER_ON_HINT));
  }

  if (renderer.isDarkMode()) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBuffer();
  }
}

void OtaUpdateActivity::beginRebootInstall() {
  LOG_DBG("OTA", "Update confirmed, arming reboot-to-install and restarting");
  OtaUpdater::requestBootInstall();
  {
    RenderLock lock(*this);
    state = REBOOTING;
  }
  requestUpdateAndWait();
  // Give the e-paper time to physically settle before the reboot wipes the
  // frame; the boot-time install flow takes over from here.
  delay(2500);
  ESP.restart();
}

void OtaUpdateActivity::runUpdateInstall() {
  LOG_DBG("OTA", "New update available, starting download...");
  {
    RenderLock lock(*this);
    state = UPDATE_IN_PROGRESS;
    lowMemoryOtaProgress = false;
    lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  }
  requestUpdateAndWait();

  LOG_DBG("OTA", "Heap before install glyph cache release: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  FontManager& fontManager = FontManager::getInstance();
  const bool atlasReady = buildOtaProgressGlyphAtlas();
  LOG_DBG("OTA", "Progress glyph atlas: %s", atlasReady ? "ready" : "unavailable");
  fontManager.releaseGlyphCaches();
  {
    // Belt-and-suspenders: resident SD font internals (interval tables, metric caches) are a
    // fragmentation source we control. When they are already unloaded (e.g. after the WiFi
    // selector path) this is a no-op; the progress UI falls back to built-in CJK glyphs and a
    // failed install reloads the configured families through the dirty flag.
    RenderLock lock(*this);
    sdFontSystem.releaseResidentFonts(renderer);
  }
  LOG_DBG("OTA", "Heap before install: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  {
    RenderLock lock(*this);
    lowMemoryOtaProgress = true;
    lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  }

  const auto res = [&]() {
    FontManager::ScopedGlyphCacheSuspension glyphCacheSuspension(fontManager);
    return updater.installUpdate(
        [](void* ctx) {
          // immediate=true notifies the render task directly. The default deferred path only
          // sets a flag consumed at the end of ActivityManager::loop(), which never runs while
          // installUpdate() blocks this task.
          static_cast<OtaUpdateActivity*>(ctx)->requestUpdate(true);
        },
        this);
  }();
  {
    RenderLock lock(*this);
    lowMemoryOtaProgress = false;
  }

  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update failed: %d", res);
    OtaUpdater::recordInstallFailureForDiagnostics(res, updater.getProcessedSize(), updater.getTotalSize());
    {
      RenderLock lock(*this);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = FINISHED;
  }
  requestUpdateAndWait();
  // Hold the completion screen briefly so the user sees it, then restart.
  delay(3000);
  {
    RenderLock lock(*this);
    state = SHUTTING_DOWN;
  }
}
void OtaUpdateActivity::loop() {
  if (state == WAITING_CONFIRMATION) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      const auto actionRects = getOtaActionRects(renderer);
      if (contains(actionRects.cancel, x, y)) {
        finish();
        return;
      }
      if (contains(actionRects.update, x, y)) {
        beginRebootInstall();
        return;
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      beginRebootInstall();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }

    return;
  }

  if (state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
    return;
  }

  if (state == NO_UPDATE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
