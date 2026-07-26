#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {

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

void renderErrorScreen(GfxRenderer& renderer, const MappedInputManager& mappedInput, const char* message) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, message);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");

  // BMP fast path — avoids the heavier ImageDecoder pipeline
  if (StringUtils::checkFileExtension(filePath, ".bmp")) {
    FsFile file;
    if (!Storage.openFileForRead("BMP", filePath, file)) {
      renderErrorScreen(renderer, mappedInput, "Could not open file");
      return;
    }

    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x = 0, y = 0, width = 0, height = 0;
      if (!calculatePlacement(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight, x, y, width, height)) {
        file.close();
        renderErrorScreen(renderer, mappedInput, "Invalid image size");
        return;
      }

      GUI.fillPopupProgress(renderer, popupRect, 50);
      renderer.clearScreen();
      renderer.beginImageRender();
      renderer.drawBitmap(bitmap, x, y, width, height, 0, 0);
      renderer.endImageRender();
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    } else {
      file.close();
      renderErrorScreen(renderer, mappedInput, "Invalid BMP file");
      return;
    }
    file.close();
    return;
  }

  // JPG / PNG path via ImageDecoderFactory
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(filePath);
  if (!decoder) {
    renderErrorScreen(renderer, mappedInput, "Unsupported image format");
    return;
  }

  ImageDimensions dimensions = {0, 0};
  if (!decoder->getDimensions(filePath, dimensions)) {
    renderErrorScreen(renderer, mappedInput, "Could not open file");
    return;
  }

  int x = 0, y = 0, width = 0, height = 0;
  if (!calculatePlacement(dimensions.width, dimensions.height, pageWidth, pageHeight, x, y, width, height)) {
    renderErrorScreen(renderer, mappedInput, "Invalid image size");
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
    renderErrorScreen(renderer, mappedInput, "Could not open file");
    return;
  }

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
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

void BmpViewerActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }
}
