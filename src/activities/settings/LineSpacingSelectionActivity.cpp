#include "LineSpacingSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kMin = 0;
constexpr int kMax = CrossPointSettings::LINE_COMPRESSION_COUNT - 1;

const char* lineSpacingLabel(int value) {
  switch (value) {
    case CrossPointSettings::TIGHT:
      return tr(STR_TIGHT);
    case CrossPointSettings::WIDE:
      return tr(STR_WIDE);
    case CrossPointSettings::NORMAL:
    default:
      return tr(STR_NORMAL);
  }
}
}  // namespace

void LineSpacingSelectionActivity::onEnter() {
  Activity::onEnter();
  if (value < kMin) {
    value = kMin;
  } else if (value > kMax) {
    value = kMax;
  }
  requestUpdate();
}

void LineSpacingSelectionActivity::onExit() { Activity::onExit(); }

void LineSpacingSelectionActivity::adjustValue(const int delta) {
  value += delta;
  if (value < kMin) {
    value = kMin;
  } else if (value > kMax) {
    value = kMax;
  }
  requestUpdate();
}

void LineSpacingSelectionActivity::loop() {
  // This sub-page is opened from Settings on Confirm *press*.
  // Using release events here would consume the same key-up and immediately exit.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    setResult(ActivityResult());  // cancelled
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    setResult(ActivityResult(PercentResult{.percent = value}));
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustValue(1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustValue(-1); });
}

void LineSpacingSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;

  renderer.drawCenteredText(UI_12_FONT_ID, 15 + hintGutterHeight, tr(STR_LINE_SPACING), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, 90 + hintGutterHeight, lineSpacingLabel(value), true, EpdFontFamily::BOLD);

  const int screenWidth = renderer.getScreenWidth();
  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = (screenWidth - barWidth) / 2;
  const int barY = 140 + hintGutterHeight;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  const int range = kMax - kMin;
  const int normalized = value - kMin;
  const int fillWidth = range > 0 ? (barWidth - 4) * normalized / range : 0;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
