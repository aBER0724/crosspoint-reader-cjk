#pragma once

#include <functional>
#include <string>

#include "../Activity.h"
#include "MappedInputManager.h"

class FontPreviewActivity final : public Activity {
 public:
  enum class ActionMask { ReaderAndUi, ReaderOnly, UiOnly };

  FontPreviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                      std::function<void()> onGoBack, ActionMask actionMask = ActionMask::ReaderAndUi);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string filePath;
  std::function<void()> onGoBack;
  ActionMask actionMask;
  int previewFontIndex = -1;
  int originalReaderFontIndex = -1;
  int originalUiFontIndex = -1;
  bool originalSelectionCaptured = false;
  bool previewUsesReaderSlot = false;
  bool previewUsesUiSlot = false;

  int findFontIndex() const;
  bool isPreviewFontSelectable() const;
  void installPreviewFont();
  void restorePreviewFont();
  void applyReaderFont();
  void applyUiFont();
  void applyReaderAndUiFont();
  void closePreview(bool applied);
};
