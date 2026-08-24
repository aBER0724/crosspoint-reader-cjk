#pragma once

#include <array>
#include <string>
#include <vector>

#include "FontInstaller.h"
#include "SdCardFont.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/FontManifest.h"

// JSON schema version of the fonts.json manifest. The canonical version for
// the build tooling lives in lib/EpdFont/scripts/cpfont_version.py. This
// firmware-side copy must be bumped manually when the firmware is updated to
// support a new manifest schema.
#define FONTS_MANIFEST_VERSION 2

#ifndef FONT_MANIFEST_URL
// Manifest + .cpfont assets are published by the independent
// aBER0724/crosspoint-cjk-fonts repository's release-fonts.yml workflow under the
// "sd-fonts-m<META>-b<BIN>" tag. The tag derives its version numbers from
// lib/EpdFont/scripts/cpfont_version.py.
#define FONT_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_URL_STRINGIFY(x) FONT_MANIFEST_URL_STRINGIFY_INNER(x)
#define FONT_MANIFEST_URL                                                                                      \
  "https://github.com/aBER0724/crosspoint-cjk-fonts/releases/download/sd-fonts-m" FONT_MANIFEST_URL_STRINGIFY( \
      FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_URL_STRINGIFY(CPFONT_VERSION) "/fonts.json"
#endif

class FontDownloadActivity : public Activity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING_PREVIEW || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING_PREVIEW,
    FONT_PREVIEW,
    DOWNLOADING,
    COMPLETE,
    ERROR,
    FAMILY_DETAIL,
  };

  enum class ErrorAction { None, Preview, Install };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // Manifest data
  std::vector<ManifestFamily> families_;
  // Raw per-repo manifest files kept on SD so families_ can be released during
  // large font-file transfers and restored (re-parsed) afterwards. Indexes and
  // merge order are preserved, so family indexes remain valid across a transfer.
  std::vector<std::string> manifestFiles_;
  bool manifestReleasedForTransfer_ = false;
  bool partialManifestFailure_ = false;
  int selectedIndex_ = 0;

  // Last-update timestamp (ISO-8601) from the most recently changed manifest,
  // used to show when the catalog content changed on the family list.
  std::string catalogUpdatedAt_;

  int previewFamilyIndex_ = -1;
  int previewFileIndex_ = -1;
  int detailFamilyIndex_ = -1;
  int activePreviewFamilyIndex_ = -1;
  int activePreviewFileIndex_ = -1;
  int previewFontId_ = 0;
  ErrorAction errorAction_ = ErrorAction::None;
  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  int downloadingFileIndex_ = -1;
  std::string errorMessage_;
  bool cancelRequested_ = false;
  bool lowMemoryDownload_ = false;
  bool wifiStarted_ = false;
  int lastProgressPercent_ = -1;
  int downloadProgressBarY_ = 0;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifests();
  bool downloadManifestToFile(const std::string& url, const char* path);
  bool parseManifestFile(const char* path, std::vector<ManifestFamily>& outFamilies, std::string& outBaseUrl,
                         std::string& outUpdatedAt, bool allowSelfHealRestart);
  void restoreManifestData();
  void openFontRepositories();
  bool pollDownloadCancellation();
  void beginNetworkTransfer(int activeFamilyIndex = -1);
  void endNetworkTransfer();
  void updateDownloadProgress(size_t downloaded, size_t total);
  void renderLowMemoryProgress();
  void refreshFamilyState(ManifestFamily& family);
  void downloadFamily(int familyIndex, int fileIndex = -1, const char* stagedFilePath = nullptr);
  void downloadAll();
  static bool parsePointSize(const char* filename, const char* familyName, uint8_t& pointSize);
  int defaultPreviewFileIndex(const ManifestFamily& family) const;
  void downloadPreview(int familyIndex, int fileIndex);
  void removePreviewTemporaryFiles();
  void closePreview();
  void returnToFamilyList();
  void installPreviewedFamily();
  void updateAll();
  bool computeFileSha256(const char* path, std::array<uint8_t, 32>& outHash);
  bool showDownloadAllRow() const;
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isFontReposRow(int index) const { return index == 0; }
  bool isDownloadAllRow(int index) const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedFamilyDeletable() const;
  bool heapSufficientForNetworkTransfer() const;
  bool detailHasDeleteRow() const;
  int detailRowCount() const;
  void promptDeleteFamily(int familyIndex);
  void promptDeleteSelectedFamily();
  void onDeleteConfirmationResult(const ActivityResult& result);
  int familyIndexFromList(int listIndex) const { return listIndex - specialRowCount(); }
  int listItemCount() const;
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};
