#pragma once

#include <cstddef>

#include "FirmwareInstaller.h"

struct SdFirmwareCandidate {
  char path[64];
  size_t size;
};

struct SdFirmwareScanResult {
  size_t candidateCount = 0;
  size_t invalidCount = 0;
};

class SdFirmwareUpdater {
 public:
  using Error = FirmwareInstaller::Error;
  using ProgressCallback = FirmwareInstaller::ProgressCallback;

  SdFirmwareScanResult scanCandidates(SdFirmwareCandidate* out, size_t maxCount);

  Error install(const char* path, ProgressCallback cb, void* ctx);

  FirmwareInstaller& installer() { return installer_; }
  const FirmwareInstaller& installer() const { return installer_; }

 private:
  enum class TryAddResult {
    NotPresent,
    Invalid,
    Added,
  };

  static TryAddResult tryAddCandidate(const char* path, SdFirmwareCandidate* out, size_t maxCount, size_t* count);

  FirmwareInstaller installer_;
};
