#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }
  lastRenderedSelectorIndex = -1;
  fullRedrawRequired = true;

  // Trigger first update
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderChapterSelectionActivity::loop() {
  const bool inputActive = mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() ||
                           mappedInput.isPressed(MappedInputManager::Button::Back) ||
                           mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                           mappedInput.isPressed(MappedInputManager::Button::Left) ||
                           mappedInput.isPressed(MappedInputManager::Button::Right) ||
                           mappedInput.isPressed(MappedInputManager::Button::Up) ||
                           mappedInput.isPressed(MappedInputManager::Button::Down) ||
                           mappedInput.isPressed(MappedInputManager::Button::NavNext) ||
                           mappedInput.isPressed(MappedInputManager::Button::NavPrevious) ||
                           mappedInput.isPressed(MappedInputManager::Button::Power) || gpio.wasTouchActivity();
  if (inputActive) {
    if (!readerInputActive) {
      activityManager.cancelCurrentRender();
      readerInputActive = true;
    }
  } else {
    readerInputActive = false;
  }

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);
  const int totalItems = getTotalItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  auto selectChapter = [this] {
    const auto tocItem = epub->getTocItem(selectorIndex);
    if (tocItem.spineIndex == -1) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    } else {
      setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
      finish();
    }
  };

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;
  switch (handleListTouch(selectorIndex, totalItems, contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      selectChapter();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    selectChapter();
  }

  buttonNavigator.onNextRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;
  const int totalItems = getTotalItems();
  const int pageItems = GUI.getListPageItems(contentHeight, false);
  const bool selectionChanged = lastRenderedSelectorIndex >= 0 && lastRenderedSelectorIndex != selectorIndex;
  const bool sameListPage = selectionChanged && lastRenderedSelectorIndex / pageItems == selectorIndex / pageItems;
  const bool partialUpdate = !renderer.isDarkMode() && !fullRedrawRequired && sameListPage;

  if (partialUpdate) {
    const int firstRow = std::min(lastRenderedSelectorIndex % pageItems, selectorIndex % pageItems);
    const int lastRow = std::max(lastRenderedSelectorIndex % pageItems, selectorIndex % pageItems);
    constexpr int selectionBleed = 2;
    const int dirtyY = contentTop + firstRow * GUI.getListRowStep(false) - selectionBleed;
    const int dirtyHeight = (lastRow - firstRow + 1) * GUI.getListRowStep(false) + selectionBleed * 2;
    renderer.fillRect(screen.x, dirtyY, screen.width, dirtyHeight, false);
    renderer.setPartialUpdateRect(screen.x, dirtyY, screen.width, dirtyHeight);
  } else {
    renderer.clearScreen();
    GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                   tr(STR_SELECT_CHAPTER));
  }

  GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, totalItems, selectorIndex,
               [this](int index) {
                 auto item = epub->getTocItem(index);
                 std::string indent((item.level - 1) * 2, ' ');
                 return indent + item.title;
               });

  if (!partialUpdate) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (partialUpdate) {
    renderer.displayBuffer();
  } else if (renderer.isDarkMode()) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBufferAsync();
  }
  lastRenderedSelectorIndex = selectorIndex;
  fullRedrawRequired = false;
}
