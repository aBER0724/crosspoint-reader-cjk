#pragma once

#include <functional>
#include <string>

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;

 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);

  // Persist the install failure's stage and heap state so a failed on-device
  // update remains diagnosable even when the failure happens with the USB
  // console unavailable (the install runs with the serial log silent for
  // minutes). reportAndClearLastInstallFailure() logs and erases the record
  // once at the next boot.
  static void recordInstallProgressForDiagnostics(uint8_t stage, size_t processed);
  static void recordInstallFailureForDiagnostics(OtaUpdaterError error, size_t processed, size_t total);
  static void reportAndClearLastInstallFailure();

  // Restart-to-clean-heap install: a fragmented app-run heap cannot hold a TLS
  // handshake plus a ~16 KiB release-CDN record simultaneously (measured -125
  // MEMORY_E mid-download with a fresh 35.5 KiB heap), so the confirm step
  // arms this flag and reboots instead of installing in place. The next boot
  // sees the flag and drives the whole flow automatically, then clears it.
  static void requestBootInstall();
  static bool consumeBootInstallRequest();
  static void clearBootInstallRequest();
};
