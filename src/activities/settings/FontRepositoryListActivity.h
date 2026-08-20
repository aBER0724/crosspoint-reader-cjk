#pragma once

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

/**
 * Manage the device's configured font repositories (GitHub "owner/repo" specs).
 *
 * The device always downloads the built-in default repository plus each
 * user-configured repository listed here, merging manifests by family name +
 * point size (earlier repository wins on conflicts). Selecting an existing
 * repository opens an Edit/Delete popup; the last virtual row adds a new one.
 */
class FontRepositoryListActivity final : public Activity {
 public:
  explicit FontRepositoryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FontRepoList", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;
  int selectedIndex_ = 0;
  int editingRepositoryIndex_ = -1;  // index of the repository being edited, -1 = add new
  bool popupClosing_ = false;
  std::string errorMessage_;

  int getItemCount() const;
  void handleSelection();
  void openEditor(int repoIndex);
  void onEditorResult(const ActivityResult& result);
  void saveSpec(std::string spec);
};
