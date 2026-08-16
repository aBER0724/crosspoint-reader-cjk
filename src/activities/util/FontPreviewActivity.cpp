#include "FontPreviewActivity.h"

#include <ExternalFont.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

std::string getDisplayName(const std::string& path) {
  const size_t slashPos = path.find_last_of('/');
  if (slashPos == std::string::npos) {
    return path;
  }
  return path.substr(slashPos + 1);
}

const char* SAMPLE_LINES[] = {
    "\xe9\xa3\x8e\xe8\xb5\xb7\xe4\xba\x91\xe8\x88\x92 \xe9\x98\x85\xe8\xaf\xbb\xe4\xb8\x8d\xe6\x85\xa2",
    "\xe6\xb1\x89\xe5\xad\x97 \xe6\x98\x8e\xe6\x9c\x9d \xe9\xbb\x91\xe4\xbd\x93 \xe6\xa5\xb7\xe4\xbd\x93",
    "\xe4\xb8\xad\xe6\x96\x87\xe6\xa0\x87\xe7\x82\xb9 "
    "\xef\xbc\x8c\xe3\x80\x82\xe3\x80\x81\xef\xbc\x9b\xef\xbc\x9a\xef\xbc\x81\xef\xbc\x9f "
    "\xe2\x80\x9c\xe2\x80\x9d\xe2\x80\x98\xe2\x80\x99",
    "\xe4\xb8\xad\xe6\x96\x87\xe6\x8b\xac\xe5\x8f\xb7 "
    "\xef\xbc\x88\xef\xbc\x89\xe3\x80\x90\xe3\x80\x91\xe3\x80\x8a\xe3\x80\x8b\xe3\x80\x8c\xe3\x80\x8d\xe2\x80\xa6\xe2"
    "\x80\x94\xef\xbf\xa5",
    "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88\xe3\x81\x8a "
    "\xe3\x82\xa2\xe3\x82\xa4\xe3\x82\xa6\xe3\x82\xa8\xe3\x82\xaa",
    "The quick brown fox jumps over 0123456789",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz",
    "Symbols: ! ? . , ; : ' \" - _ + = / \\ | @ # $ % & *",
    "Brackets: ( ) [ ] { } < >",
};
constexpr size_t SAMPLE_LINE_COUNT = sizeof(SAMPLE_LINES) / sizeof(SAMPLE_LINES[0]);
static_assert(SAMPLE_LINE_COUNT >= 6);

}  // namespace

FontPreviewActivity::FontPreviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                         std::function<void()> onGoBack, ActionMask actionMask)
    : Activity("FontPreview", renderer, mappedInput),
      filePath(std::move(path)),
      onGoBack(std::move(onGoBack)),
      actionMask(actionMask) {}

void FontPreviewActivity::onEnter() {
  LOG_DBG("FNTPREV", "onEnter start");
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const std::string displayName = getDisplayName(filePath);

  renderer.clearScreen();
  LOG_DBG("FNTPREV", "drawHeader...");
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, displayName.c_str(),
                 tr(STR_PREVIEW));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 20;
  const int previewFontId = (actionMask == ActionMask::UiOnly) ? UI_12_FONT_ID : SETTINGS.getBuiltInReaderFontId();
  const int lineSpacing = renderer.getLineHeight(previewFontId) + 6;
  const Rect contentRect = GUI.getContentRect(renderer, contentTop, metrics.verticalSpacing);
  const int contentBottom = contentRect.y + contentRect.height;
  const int contentLeft = contentRect.x + metrics.contentSidePadding;
  const int contentWidth = contentRect.width - metrics.contentSidePadding * 2;
  const int maxLines = (lineSpacing > 0) ? ((contentBottom - contentTop) / lineSpacing) : 0;

  installPreviewFont();
  LOG_DBG("FNTPREV", "draw sample text...");
  int y = contentTop;
  int drawnLines = 0;
  bool isFirstSample = true;
  for (const char* sampleLine : SAMPLE_LINES) {
    if (drawnLines >= maxLines) {
      break;
    }
    const EpdFontFamily::Style style = isFirstSample ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int remainingLines = maxLines - drawnLines;
    const auto wrappedLines = renderer.wrappedText(previewFontId, sampleLine, contentWidth, remainingLines, style);
    for (const auto& wrappedLine : wrappedLines) {
      renderer.drawText(previewFontId, contentLeft, y, wrappedLine.c_str(), true, style);
      y += lineSpacing;
      ++drawnLines;
      if (drawnLines >= maxLines) {
        break;
      }
    }
    isFirstSample = false;
  }
  restorePreviewFont();

  LOG_DBG("FNTPREV", "drawButtonHints...");
  const bool chooseScope = actionMask == ActionMask::ReaderAndUi;
  const char* confirmLabel =
      chooseScope ? tr(STR_READER_AND_UI)
                  : (actionMask == ActionMask::ReaderOnly ? tr(STR_EXT_READER_FONT) : tr(STR_EXT_UI_FONT));
  const char* previousLabel = chooseScope ? tr(STR_EXT_READER_FONT) : "";
  const char* nextLabel = chooseScope ? tr(STR_EXT_UI_FONT) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, previousLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  LOG_DBG("FNTPREV", "displayBuffer FAST_REFRESH...");
  renderer.displayBuffer();
  LOG_DBG("FNTPREV", "onEnter done");
}

void FontPreviewActivity::onExit() { Activity::onExit(); }

void FontPreviewActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closePreview(false);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (actionMask == ActionMask::ReaderAndUi) {
      applyReaderAndUiFont();
    } else if (actionMask == ActionMask::UiOnly) {
      applyUiFont();
    } else {
      applyReaderFont();
    }
    return;
  }

  if (actionMask == ActionMask::ReaderAndUi && mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    applyReaderFont();
    return;
  }

  if (actionMask == ActionMask::ReaderAndUi && mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    applyUiFont();
    return;
  }
}

int FontPreviewActivity::findFontIndex() const {
  const std::string displayName = getDisplayName(filePath);
  FontMgr.scanFonts();

  for (int i = 0; i < FontMgr.getFontCount(); ++i) {
    const FontInfo* info = FontMgr.getFontInfo(i);
    if (info && displayName == info->filename) {
      return i;
    }
  }

  return -1;
}

bool FontPreviewActivity::isPreviewFontSelectable() const {
  const FontInfo* info = FontMgr.getFontInfo(previewFontIndex);
  return info && ExternalFont::canFitGlyph(info->width, info->height);
}

void FontPreviewActivity::installPreviewFont() {
  originalReaderFontIndex = FontMgr.getSelectedIndex();
  originalUiFontIndex = FontMgr.getUiSelectedIndex();
  previewFontIndex = findFontIndex();

  if (!isPreviewFontSelectable()) {
    LOG_DBG("FNTPREV", "Preview font not selectable: %s", filePath.c_str());
    return;
  }

  if (actionMask == ActionMask::UiOnly) {
    previewUsesUiSlot = FontMgr.previewUiFont(previewFontIndex);
    return;
  }

  previewUsesReaderSlot = FontMgr.previewFont(previewFontIndex);
  renderer.setReaderFallbackFontId(SETTINGS.getBuiltInReaderFontId());
}

void FontPreviewActivity::restorePreviewFont() {
  if (previewUsesReaderSlot || previewUsesUiSlot) {
    FontMgr.restoreFontSelection(originalReaderFontIndex, originalUiFontIndex);
    renderer.setReaderFallbackFontId(SETTINGS.getBuiltInReaderFontId());
  }
}

void FontPreviewActivity::applyReaderFont() {
  if (!isPreviewFontSelectable()) {
    LOG_DBG("FNTPREV", "Reader font not selectable: %s", filePath.c_str());
    return;
  }

  FontMgr.restoreFontSelection(originalReaderFontIndex, originalUiFontIndex);
  FontMgr.selectFont(previewFontIndex);
  renderer.setReaderFallbackFontId(SETTINGS.getBuiltInReaderFontId());
  closePreview(true);
}

void FontPreviewActivity::applyUiFont() {
  if (!isPreviewFontSelectable()) {
    LOG_DBG("FNTPREV", "UI font not selectable: %s", filePath.c_str());
    return;
  }

  FontMgr.restoreFontSelection(originalReaderFontIndex, originalUiFontIndex);
  FontMgr.selectUiFont(previewFontIndex);
  renderer.setReaderFallbackFontId(SETTINGS.getBuiltInReaderFontId());
  closePreview(true);
}

void FontPreviewActivity::applyReaderAndUiFont() {
  if (!isPreviewFontSelectable()) {
    LOG_DBG("FNTPREV", "Reader/UI font not selectable: %s", filePath.c_str());
    return;
  }

  FontMgr.restoreFontSelection(originalReaderFontIndex, originalUiFontIndex);
  FontMgr.selectFont(previewFontIndex);
  FontMgr.selectUiFont(previewFontIndex);
  renderer.setReaderFallbackFontId(SETTINGS.getBuiltInReaderFontId());
  closePreview(true);
}

void FontPreviewActivity::closePreview(bool applied) {
  ActivityResult result;
  result.isCancelled = !applied;
  setResult(std::move(result));
  if (!applied) {
    restorePreviewFont();
  }

  if (onGoBack) {
    onGoBack();
  } else {
    finish();
  }
}
