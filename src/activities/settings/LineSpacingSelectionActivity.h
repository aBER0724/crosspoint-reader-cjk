#pragma once

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "activities/ActivityResult.h"
#include "util/ButtonNavigator.h"

// Legacy continuous-percent UI retained as a compact enum selector for the
// upstream LINE_COMPRESSION model (TIGHT/NORMAL/WIDE). TextSettingsActivity is
// the primary editor; this activity remains for any residual callers.
class LineSpacingSelectionActivity final : public Activity {
 public:
  explicit LineSpacingSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int initialValue)
      : Activity("LineSpacingSelection", renderer, mappedInput), value(initialValue) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int value = 1;  // CrossPointSettings::NORMAL
  ButtonNavigator buttonNavigator;

  void adjustValue(int delta);
};
