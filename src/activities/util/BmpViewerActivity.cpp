#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FileTypeUtils.h"
#include "util/StringUtils.h"

namespace {

constexpr char SLEEP_COVER_PATH[] = "/sleep.bmp";
constexpr char TEMP_SLEEP_COVER_PATH[] = "/sleep.bmp.tmp";
constexpr size_t COPY_BUFFER_SIZE = 2048;

bool calculatePlacement(const int sourceWidth, const int sourceHeight, const int pageWidth, const int pageHeight,
                        int& x, int& y, int& width, int& height) {
  if (sourceWidth <= 0 || sourceHeight <= 0) {
    return false;
  }

  width = sourceWidth;
  height = sourceHeight;
  if (sourceWidth > pageWidth || sourceHeight > pageHeight) {
    const float scaleX = static_cast<float>(pageWidth) / static_cast<float>(sourceWidth);
    const float scaleY = static_cast<float>(pageHeight) / static_cast<float>(sourceHeight);
    const float scale = std::min(scaleX, scaleY);
    width = std::max(1, static_cast<int>(std::round(sourceWidth * scale)));
    height = std::max(1, static_cast<int>(std::round(sourceHeight * scale)));
  }

  x = (pageWidth - width) / 2;
  y = (pageHeight - height) / 2;
  return true;
}

void renderErrorScreen(GfxRenderer& renderer, const MappedInputManager::Labels& labels, const char* message) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, message);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (renderer.isDarkMode()) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }
}

}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) {
    return;
  }

  const std::string dirPath = FsHelpers::extractFolderPath(filePath);
  const size_t lastSlash = filePath.find_last_of('/');
  const std::string fileName = lastSlash == std::string::npos ? filePath : filePath.substr(lastSlash + 1);

  HalFile dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return;
  }

  char name[500];
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    name[0] = '\0';
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.' && FileTypeUtils::isDirectlyViewableImageFile(name)) {
        siblingImages.emplace_back(name);
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);
  for (size_t i = 0; i < siblingImages.size(); ++i) {
    if (siblingImages[i] == fileName) {
      currentImageIndex = static_cast<int>(i);
      break;
    }
  }
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  const bool hasPrevious = siblingImages.size() > 1 && currentImageIndex > 0;
  const bool hasNext = siblingImages.size() > 1 && currentImageIndex >= 0 &&
                       currentImageIndex < static_cast<int>(siblingImages.size()) - 1;
  const bool isBmp = StringUtils::checkFileExtension(filePath, ".bmp");
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), isBmp ? tr(STR_SET_SLEEP_COVER) : "",
                                            hasPrevious ? "<" : "", hasNext ? ">" : "");

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);

  // BMP fast path avoids the heavier ImageDecoder pipeline.
  if (isBmp) {
    HalFile file;
    if (!Storage.openFileForRead("BMP", filePath, file)) {
      renderErrorScreen(renderer, labels, "Could not open file");
      return;
    }

    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) {
      file.close();
      renderErrorScreen(renderer, labels, "Invalid BMP file");
      return;
    }

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (!calculatePlacement(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight, x, y, width, height)) {
      file.close();
      renderErrorScreen(renderer, labels, "Invalid image size");
      return;
    }

    GUI.fillPopupProgress(renderer, popupRect, 50);
    renderer.clearScreen();
    renderer.beginImageRender();
    renderer.drawBitmap(bitmap, x, y, width, height, 0, 0);
    renderer.endImageRender();
    file.close();

    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (renderer.isDarkMode()) {
      renderer.displayBufferDarkRedrive();
    } else {
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    }
    return;
  }

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(filePath);
  if (!decoder) {
    renderErrorScreen(renderer, labels, "Unsupported image format");
    return;
  }

  ImageDimensions dimensions = {0, 0};
  if (!decoder->getDimensions(filePath, dimensions)) {
    renderErrorScreen(renderer, labels, "Could not open file");
    return;
  }

  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  if (!calculatePlacement(dimensions.width, dimensions.height, pageWidth, pageHeight, x, y, width, height)) {
    renderErrorScreen(renderer, labels, "Invalid image size");
    return;
  }

  GUI.fillPopupProgress(renderer, popupRect, 50);
  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;
  config.cachePath.clear();

  renderer.clearScreen();
  renderer.beginImageRender();
  const bool success = decoder->decodeToFramebuffer(filePath, renderer, config);
  renderer.endImageRender();
  if (!success) {
    renderErrorScreen(renderer, labels, "Could not open file");
    return;
  }

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (renderer.isDarkMode()) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  if (renderer.isDarkMode()) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  bool success = false;
  if (!StringUtils::checkFileExtension(filePath, ".bmp")) {
    LOG_ERR("BMP", "Refusing non-BMP sleep cover: %s", filePath.c_str());
  } else {
    HalFile validationFile;
    if (!Storage.openFileForRead("BMP", filePath, validationFile)) {
      LOG_ERR("BMP", "Could not open sleep cover source: %s", filePath.c_str());
    } else {
      Bitmap bitmap(validationFile, true);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        LOG_ERR("BMP", "Invalid BMP sleep cover source: %s", filePath.c_str());
      } else {
        success = true;
      }
      validationFile.close();
    }
  }

  if (success && filePath != SLEEP_COVER_PATH) {
    success = false;
    HalFile inFile;
    if (!Storage.openFileForRead("BMP", filePath, inFile)) {
      LOG_ERR("BMP", "Could not reopen sleep cover source: %s", filePath.c_str());
    } else {
      const bool tempReady = !Storage.exists(TEMP_SLEEP_COVER_PATH) || Storage.remove(TEMP_SLEEP_COVER_PATH);
      if (!tempReady) {
        LOG_ERR("BMP", "Could not remove stale temporary sleep cover");
      } else {
        HalFile outFile;
        if (!Storage.openFileForWrite("BMP", TEMP_SLEEP_COVER_PATH, outFile)) {
          LOG_ERR("BMP", "Could not open temporary sleep cover for writing");
        } else {
          char buffer[COPY_BUFFER_SIZE];
          bool copySuccess = true;
          while (copySuccess) {
            const int bytesRead = inFile.read(buffer, sizeof(buffer));
            if (bytesRead < 0) {
              LOG_ERR("BMP", "Failed reading sleep cover source");
              copySuccess = false;
            } else if (bytesRead == 0) {
              break;
            } else if (outFile.write(buffer, static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) {
              LOG_ERR("BMP", "Failed writing temporary sleep cover");
              copySuccess = false;
            }
          }

          outFile.flush();
          if (!outFile.close()) {
            LOG_ERR("BMP", "Failed closing temporary sleep cover");
            copySuccess = false;
          }
          inFile.close();

          if (!copySuccess) {
            Storage.remove(TEMP_SLEEP_COVER_PATH);
          } else if (Storage.exists(SLEEP_COVER_PATH) && !Storage.remove(SLEEP_COVER_PATH)) {
            LOG_ERR("BMP", "Could not remove existing sleep cover");
            Storage.remove(TEMP_SLEEP_COVER_PATH);
          } else if (!Storage.rename(TEMP_SLEEP_COVER_PATH, SLEEP_COVER_PATH)) {
            LOG_ERR("BMP", "Could not install temporary sleep cover");
          } else {
            success = true;
          }
        }
      }
      if (inFile) {
        inFile.close();
      }
    }
  }

  if (success) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    if (!SETTINGS.saveToFile()) {
      LOG_ERR("BMP", "Failed to save custom sleep cover setting");
      success = false;
    }
  }

  GUI.drawPopup(renderer, success ? tr(STR_DONE) : tr(STR_FAILED_LOWER));
  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  Activity::loop();

  auto openSibling = [this](const int delta) {
    if (currentImageIndex < 0) {
      return false;
    }

    const int nextIndex = currentImageIndex + delta;
    if (siblingImages.size() <= 1 || nextIndex < 0 || nextIndex >= static_cast<int>(siblingImages.size())) {
      return false;
    }

    currentImageIndex = nextIndex;
    std::string dirPath = FsHelpers::extractFolderPath(filePath);
    if (dirPath.back() != '/') {
      dirPath += '/';
    }
    filePath = dirPath + siblingImages[currentImageIndex];
    onEnter();
    return true;
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openSibling(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    openSibling(-1);
    return;
  }

  if (StringUtils::checkFileExtension(filePath, ".bmp") &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doSetSleepCover();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
  }
}
