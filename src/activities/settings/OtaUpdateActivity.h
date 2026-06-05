#pragma once

#include "activities/Activity.h"
#include "network/OtaUpdater.h"

class OtaUpdateActivity : public Activity {
  enum State {
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    UPDATE_IN_PROGRESS,
    NO_UPDATE,
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };

  // Can't initialize this to 0 or the first render doesn't happen
  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;
  static constexpr int OTA_PROGRESS_GLYPH_COUNT = 12;
  static constexpr int OTA_PROGRESS_GLYPH_MAX_BYTES = 260;

  struct OtaProgressGlyph {
    char ch = '\0';
    uint8_t width = 0;
    uint8_t height = 0;
    uint8_t bytesPerRow = 0;
    uint8_t advance = 0;
    uint8_t bitmap[OTA_PROGRESS_GLYPH_MAX_BYTES] = {};
  };

  State state = WIFI_SELECTION;
  unsigned int lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  bool lowMemoryOtaProgress = false;
  bool otaProgressGlyphAtlasReady = false;
  OtaProgressGlyph otaProgressGlyphs[OTA_PROGRESS_GLYPH_COUNT];
  OtaUpdater updater;

  void onWifiSelectionComplete(bool success);
  bool buildOtaProgressGlyphAtlas();
  const OtaProgressGlyph* findOtaProgressGlyph(char ch) const;
  void drawOtaProgressText(const char* text, int y);
  void renderOtaProgressOnly(unsigned int percentage, size_t processedSize, size_t totalSize);

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OtaUpdate", renderer, mappedInput), updater() {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CHECKING_FOR_UPDATE || state == UPDATE_IN_PROGRESS; }
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
};
