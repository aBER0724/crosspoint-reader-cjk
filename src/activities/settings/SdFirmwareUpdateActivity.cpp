#include "SdFirmwareUpdateActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
void formatFileSize(char* buf, const size_t bufSize, const size_t bytes) {
  if (!buf || bufSize == 0) {
    return;
  }

  if (bytes >= 1024 * 1024) {
    snprintf(buf, bufSize, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, bufSize, "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, bufSize, "%u B", static_cast<unsigned>(bytes));
  }
}

std::string formatFileSize(const size_t bytes) {
  char buf[32];
  formatFileSize(buf, sizeof(buf), bytes);
  return buf;
}
}  // namespace

void SdFirmwareUpdateActivity::onInstallProgress(const size_t processed, const size_t total, void* ctx) {
  auto* self = static_cast<SdFirmwareUpdateActivity*>(ctx);
  if (!self || total == 0) {
    return;
  }

  const int progressHalf = static_cast<int>((processed * 50) / total);
  if (progressHalf == self->lastProgressHalf) {
    return;
  }
  self->lastProgressHalf = progressHalf;
  self->requestUpdate(true);
}

const char* SdFirmwareUpdateActivity::basenameFromPath(const char* path) const {
  if (!path) {
    return "";
  }
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

const char* SdFirmwareUpdateActivity::errorMessageFor(const SdFirmwareUpdater::Error error) const {
  switch (error) {
    case SdFirmwareUpdater::Error::INVALID_IMAGE:
      return tr(STR_SD_FIRMWARE_ERR_INVALID);
    case SdFirmwareUpdater::Error::READ_ERROR:
      return tr(STR_SD_FIRMWARE_ERR_READ);
    case SdFirmwareUpdater::Error::IMAGE_TOO_LARGE:
      return tr(STR_SD_FIRMWARE_ERR_TOO_LARGE);
    case SdFirmwareUpdater::Error::NO_PARTITION:
      return tr(STR_SD_FIRMWARE_ERR_PARTITION);
    case SdFirmwareUpdater::Error::OTA_BEGIN_FAILED:
    case SdFirmwareUpdater::Error::OTA_WRITE_FAILED:
    case SdFirmwareUpdater::Error::OTA_END_FAILED:
      return tr(STR_SD_FIRMWARE_ERR_FLASH);
    case SdFirmwareUpdater::Error::OK:
      break;
  }
  return tr(STR_UPDATE_FAILED);
}

void SdFirmwareUpdateActivity::runScan() {
  const SdFirmwareScanResult scanResult = updater.scanCandidates(candidates, kMaxCandidates);
  candidateCount = scanResult.candidateCount;
  invalidCandidateCount = scanResult.invalidCount;
  scanDone = true;

  if (candidateCount == 0) {
    state = NO_FILE;
    return;
  }

  if (candidateCount == 1) {
    selectCandidate(0);
    state = CONFIRM;
    return;
  }

  selectedIndex = 0;
  state = SELECT_FILE;
}

void SdFirmwareUpdateActivity::selectCandidate(const int index) {
  if (index < 0 || static_cast<size_t>(index) >= candidateCount) {
    return;
  }
  strncpy(selectedPath, candidates[index].path, sizeof(selectedPath) - 1);
  selectedPath[sizeof(selectedPath) - 1] = '\0';
  selectedSize = candidates[index].size;
}

void SdFirmwareUpdateActivity::startInstall() {
  LOG_DBG("SD_OTA", "Starting install: %s", selectedPath);
  lastProgressHalf = -1;
  lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  lastInstallError = SdFirmwareUpdater::Error::OK;

  {
    RenderLock lock;
    state = INSTALLING;
  }
  requestUpdateAndWait();

  const auto res = updater.install(selectedPath, onInstallProgress, this);

  if (res != SdFirmwareUpdater::Error::OK) {
    LOG_ERR("SD_OTA", "Install failed: %d", static_cast<int>(res));
    lastInstallError = res;
    {
      RenderLock lock;
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock;
    state = FINISHED;
  }
  requestUpdate();
}

void SdFirmwareUpdateActivity::onEnter() {
  Activity::onEnter();
  scanDone = false;
  candidateCount = 0;
  invalidCandidateCount = 0;
  selectedIndex = 0;
  selectedPath[0] = '\0';
  selectedSize = 0;
  lastInstallError = SdFirmwareUpdater::Error::OK;
  lastProgressHalf = -1;
  state = SCANNING;
  requestUpdate();
}

void SdFirmwareUpdateActivity::onExit() { Activity::onExit(); }

void SdFirmwareUpdateActivity::loop() {
  if (!scanDone && state == SCANNING) {
    runScan();
    requestUpdate();
    return;
  }

  if (state == SELECT_FILE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      selectCandidate(selectedIndex);
      {
        RenderLock lock;
        state = CONFIRM;
      }
      requestUpdate();
      return;
    }

    buttonNavigator.onNextRelease([this] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(candidateCount));
      requestUpdate();
    });

    buttonNavigator.onPreviousRelease([this] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(candidateCount));
      requestUpdate();
    });
    return;
  }

  if (state == CONFIRM) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      startInstall();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (candidateCount > 1) {
        {
          RenderLock lock;
          state = SELECT_FILE;
        }
        requestUpdate();
      } else {
        finish();
      }
    }
    return;
  }

  if (state == FAILED || state == NO_FILE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == FINISHED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock;
        state = SHUTTING_DOWN;
      }
      requestUpdate();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}

void SdFirmwareUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_INSTALL_FIRMWARE_SD));

  if (state == SCANNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SD_FIRMWARE_SCANNING));
    renderer.displayBuffer();
    return;
  }

  if (state == NO_FILE) {
    const char* message = invalidCandidateCount > 0 ? tr(STR_SD_FIRMWARE_INVALID_FOUND) : tr(STR_SD_FIRMWARE_NOT_FOUND);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, message, true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SELECT_FILE) {
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop, tr(STR_SD_FIRMWARE_SELECT), true);
    const Rect contentRect =
        GUI.getContentRect(renderer, contentTop + height + metrics.verticalSpacing, metrics.verticalSpacing * 2);
    GUI.drawList(
        renderer, contentRect, static_cast<int>(candidateCount), selectedIndex,
        [this](int index) { return basenameFromPath(candidates[index].path); }, nullptr, nullptr,
        [this](int index) { return formatFileSize(candidates[index].size); }, true);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CONFIRM) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 80, tr(STR_SD_FIRMWARE_CONFIRM_TITLE), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 50, tr(STR_SD_FIRMWARE_WARNING_1));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_SD_FIRMWARE_WARNING_2));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, tr(STR_SD_FIRMWARE_WARNING_3));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_SD_FIRMWARE_WARNING_4));

    char fileLine[96];
    snprintf(fileLine, sizeof(fileLine), "%s %s", tr(STR_SD_FIRMWARE_FILE), basenameFromPath(selectedPath));

    char sizeText[32];
    formatFileSize(sizeText, sizeof(sizeText), selectedSize);
    char sizeLine[48];
    snprintf(sizeLine, sizeof(sizeLine), "%s %s", tr(STR_SD_FIRMWARE_SIZE), sizeText);

    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 40, fileLine);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 60, sizeLine);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto top = (pageHeight - height) / 2;

  if (state == INSTALLING) {
    float updaterProgress = 0;
    const size_t processed = updater.installer().getProcessedSize();
    const size_t total = updater.installer().getTotalSize();
    if (total > 0) {
      updaterProgress = static_cast<float>(processed) / static_cast<float>(total);
      if (static_cast<int>(updaterProgress * 50) == static_cast<int>(lastUpdaterPercentage) / 2) {
        return;
      }
      lastUpdaterPercentage = static_cast<int>(updaterProgress * 100);
    }

    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING));
    int y = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(updaterProgress * 100), 100);
    y += metrics.progressBarHeight + metrics.verticalSpacing;
    char percentText[8];
    snprintf(percentText, sizeof(percentText), "%d%%", static_cast<int>(updaterProgress * 100));
    renderer.drawCenteredText(UI_10_FONT_ID, y, percentText);
    y += height + metrics.verticalSpacing;

    char bytesText[32];
    snprintf(bytesText, sizeof(bytesText), "%u / %u", static_cast<unsigned>(processed), static_cast<unsigned>(total));
    renderer.drawCenteredText(UI_10_FONT_ID, y, bytesText);
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SD_FIRMWARE_DO_NOT_POWER_OFF), true);
    const auto labels = mappedInput.mapLabels("", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, errorMessageFor(lastInstallError));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing,
                              tr(STR_SD_FIRMWARE_PRESS_CONFIRM_RESTART));
    const auto labels = mappedInput.mapLabels("", tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SHUTTING_DOWN) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
  }
}
