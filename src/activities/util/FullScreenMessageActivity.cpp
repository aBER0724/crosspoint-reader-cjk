#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>

#include "fontIds.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (renderer.getScreenHeight() - height) / 2;

  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, top, text.c_str(), true, style);
  if (renderer.isDarkMode() && (refreshMode == HalDisplay::FAST_REFRESH || refreshMode == HalDisplay::HALF_REFRESH)) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBuffer(refreshMode);
  }
}
