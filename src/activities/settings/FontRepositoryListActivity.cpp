#include "FontRepositoryListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "FontRepositoryStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FontRepositoryUtil.h"

namespace {
// Trims surrounding whitespace from a user-entered repository spec.
std::string trimWhitespace(std::string v) {
  const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!v.empty() && isSpace(v.front())) v.erase(v.begin());
  while (!v.empty() && isSpace(v.back())) v.pop_back();
  return v;
}
}  // namespace

int FontRepositoryListActivity::getItemCount() const {
  return static_cast<int>(FONT_REPO_STORE.getCount()) + 1;  // + Add repository row
}

void FontRepositoryListActivity::onEnter() {
  Activity::onEnter();

  // Reload from disk in case repositories changed while a subactivity was open.
  FONT_REPO_STORE.loadFromFile();
  selectedIndex_ = 0;
  errorMessage_.clear();
  requestUpdate();
}

void FontRepositoryListActivity::onExit() { Activity::onExit(); }

void FontRepositoryListActivity::loop() {
  // A modal OptionPopup (Edit/Delete) consumes input while open.
  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) {
    popupClosing_ = !optionPopup_.isActive();
    return;
  }
  if (popupClosing_) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      return;  // closing press still held
    }
    popupClosing_ = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      return;  // swallow the release that closed the popup
    }
  }

  auto activateSelected = [this] { handleSelection(); };

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  const int itemCount = getItemCount();
  if (itemCount > 0) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const Rect contentRect = GUI.getContentRect(renderer, contentTop, metrics.verticalSpacing * 2);
    switch (handleListTouch(selectedIndex_, itemCount, contentRect.y, contentRect.height, true)) {
      case ListTouchResult::Activated:
        activateSelected();
        return;
      case ListTouchResult::Consumed:
        return;
      case ListTouchResult::None:
        break;
    }

    const int pageItems = GUI.getListPageItems(contentRect.height, true);
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up) {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, itemCount, pageItems);
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, itemCount, pageItems);
      requestUpdate();
      return;
    }

    buttonNavigator_.onNext([this, itemCount] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, itemCount);
      requestUpdate();
    });

    buttonNavigator_.onPrevious([this, itemCount] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, itemCount);
      requestUpdate();
    });
  }
}

void FontRepositoryListActivity::handleSelection() {
  const auto repoCount = static_cast<int>(FONT_REPO_STORE.getCount());

  // Selecting an existing repository opens an Edit/Delete popup.
  if (selectedIndex_ < repoCount) {
    editingRepositoryIndex_ = selectedIndex_;
    static const StrId options[] = {StrId::STR_FONT_REPOSITORY_EDIT, StrId::STR_FONT_REPOSITORY_DELETE};
    optionPopup_.show(StrId::STR_FONT_REPOSITORIES, options, 2, 0, [this](int idx) {
      const size_t repoIndex = static_cast<size_t>(editingRepositoryIndex_);
      if (idx == 0) {
        openEditor(editingRepositoryIndex_);
      } else if (idx == 1) {
        FONT_REPO_STORE.removeRepository(repoIndex);
        errorMessage_.clear();
        requestUpdate();
      }
    });
    requestUpdate();
    return;
  }

  // Add repository virtual row.
  openEditor(-1);
}

void FontRepositoryListActivity::openEditor(int repoIndex) {
  std::string initial;
  if (repoIndex >= 0) {
    const auto* repo = FONT_REPO_STORE.getRepository(static_cast<size_t>(repoIndex));
    if (repo) initial = *repo;
  }
  editingRepositoryIndex_ = repoIndex;
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(
                             renderer, mappedInput, tr(STR_FONT_REPOSITORY_ENTRY_TITLE), initial, 160, InputType::Text),
                         [this](const ActivityResult& result) { onEditorResult(result); });
}

void FontRepositoryListActivity::onEditorResult(const ActivityResult& result) {
  if (result.isCancelled) return;
  const auto& kb = std::get<KeyboardResult>(result.data);
  saveSpec(kb.text);
}

void FontRepositoryListActivity::saveSpec(std::string spec) {
  spec = trimWhitespace(std::move(spec));

  if (!isValidRepositorySpec(spec)) {
    errorMessage_ = tr(STR_FONT_REPOSITORY_INVALID);
    requestUpdate();
    return;
  }

  bool saved = false;
  if (editingRepositoryIndex_ >= 0) {
    saved = FONT_REPO_STORE.updateRepository(static_cast<size_t>(editingRepositoryIndex_), spec);
  } else {
    saved = FONT_REPO_STORE.addRepository(spec);
  }

  if (!saved) {
    errorMessage_ = tr(STR_FONT_REPO_LIMIT);
    requestUpdate();
    return;
  }

  errorMessage_.clear();
  requestUpdate();
}

void FontRepositoryListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_FONT_REPOSITORIES));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const Rect contentRect = GUI.getContentRect(renderer, contentTop, metrics.verticalSpacing * 2);
  const int itemCount = getItemCount();

  // A transient error hint (invalid spec / limit reached) is drawn above the list.
  int listTop = contentTop;
  if (!errorMessage_.empty()) {
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, listTop, errorMessage_.c_str(), true);
    listTop += lineHeight + metrics.verticalSpacing;
  }
  const int listHeight = contentRect.y + contentRect.height - listTop;

  if (itemCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_FONT_REPOSITORIES));
  } else {
    const auto& repos = FONT_REPO_STORE.getRepositories();
    const auto repoCount = static_cast<int>(repos.size());

    GUI.drawList(
        renderer, Rect{0, listTop, pageWidth, listHeight}, itemCount, selectedIndex_,
        [&repos, repoCount](int index) -> std::string {
          if (index < repoCount) {
            return repos[index];
          }
          return std::string(I18n::getInstance().get(StrId::STR_ADD_FONT_REPOSITORY));
        },
        [repoCount](int index) -> std::string {
          if (index == repoCount) {
            return std::string(I18n::getInstance().get(StrId::STR_FONT_REPOSITORY_ENTRY_TITLE));
          }
          return std::string("");
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (optionPopup_.processRender(renderer, mappedInput)) {
    return;  // popup handled the display (including displayBuffer)
  }

  renderer.displayBuffer();
}
