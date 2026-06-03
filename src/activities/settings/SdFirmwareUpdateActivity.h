#pragma once

#include "activities/Activity.h"
#include "network/SdFirmwareUpdater.h"
#include "util/ButtonNavigator.h"

class SdFirmwareUpdateActivity final : public Activity {
 public:
  explicit SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SdFirmwareUpdate", renderer, mappedInput), candidates{} {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == SCANNING || state == INSTALLING || state == FINISHED || state == SHUTTING_DOWN;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    SCANNING,
    NO_FILE,
    SELECT_FILE,
    CONFIRM,
    INSTALLING,
    FAILED,
    FINISHED,
    SHUTTING_DOWN,
  };

  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;
  static constexpr size_t kMaxCandidates = 3;

  State state = SCANNING;
  bool scanDone = false;
  unsigned int lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  int lastProgressHalf = -1;

  SdFirmwareUpdater updater;
  SdFirmwareUpdater::Error lastInstallError = SdFirmwareUpdater::Error::OK;
  SdFirmwareCandidate candidates[kMaxCandidates];
  size_t candidateCount = 0;
  size_t invalidCandidateCount = 0;
  int selectedIndex = 0;
  char selectedPath[64] = {};
  size_t selectedSize = 0;

  ButtonNavigator buttonNavigator;

  void runScan();
  void selectCandidate(int index);
  void startInstall();
  static void onInstallProgress(size_t processed, size_t total, void* ctx);

  const char* basenameFromPath(const char* path) const;
  const char* errorMessageFor(SdFirmwareUpdater::Error error) const;
};
