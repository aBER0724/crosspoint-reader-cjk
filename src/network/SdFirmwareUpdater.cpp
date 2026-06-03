#include "SdFirmwareUpdater.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace {
constexpr const char* kCandidatePaths[] = {"/firmware-sc.bin", "/firmware-tc.bin", "/firmware.bin"};
constexpr size_t kCandidateCount = sizeof(kCandidatePaths) / sizeof(kCandidatePaths[0]);
}  // namespace

SdFirmwareUpdater::TryAddResult SdFirmwareUpdater::tryAddCandidate(const char* path, SdFirmwareCandidate* out,
                                                                   const size_t maxCount, size_t* count) {
  if (*count >= maxCount) {
    return TryAddResult::NotPresent;
  }

  if (!Storage.exists(path)) {
    return TryAddResult::NotPresent;
  }

  HalFile file;
  if (!Storage.openFileForRead("SD_OTA", path, file)) {
    LOG_ERR("SD_OTA", "Failed to open candidate: %s", path);
    return TryAddResult::Invalid;
  }

  if (file.isDirectory()) {
    file.close();
    return TryAddResult::Invalid;
  }

  const size_t size = file.fileSize();
  if (size == 0) {
    LOG_ERR("SD_OTA", "Empty firmware file: %s", path);
    file.close();
    return TryAddResult::Invalid;
  }

  if (size > FirmwareInstaller::kMaxAppImageSize) {
    LOG_ERR("SD_OTA", "Firmware file too large: %s (%u bytes)", path, static_cast<unsigned>(size));
    file.close();
    return TryAddResult::Invalid;
  }

  if (FirmwareInstaller::validateImageHeader(file) != FirmwareInstaller::Error::OK) {
    LOG_ERR("SD_OTA", "Invalid firmware image: %s", path);
    file.close();
    return TryAddResult::Invalid;
  }

  file.close();

  SdFirmwareCandidate& candidate = out[*count];
  strncpy(candidate.path, path, sizeof(candidate.path) - 1);
  candidate.path[sizeof(candidate.path) - 1] = '\0';
  candidate.size = size;
  (*count)++;

  LOG_DBG("SD_OTA", "Found candidate: %s (%u bytes)", path, static_cast<unsigned>(size));
  return TryAddResult::Added;
}

SdFirmwareScanResult SdFirmwareUpdater::scanCandidates(SdFirmwareCandidate* out, const size_t maxCount) {
  SdFirmwareScanResult result;
  if (!out || maxCount == 0) {
    return result;
  }

  if (!Storage.ready()) {
    LOG_ERR("SD_OTA", "SD card not ready");
    return result;
  }

  for (size_t i = 0; i < kCandidateCount; i++) {
    switch (tryAddCandidate(kCandidatePaths[i], out, maxCount, &result.candidateCount)) {
      case TryAddResult::Added:
      case TryAddResult::NotPresent:
        break;
      case TryAddResult::Invalid:
        result.invalidCount++;
        break;
    }
  }

  return result;
}

SdFirmwareUpdater::Error SdFirmwareUpdater::install(const char* path, ProgressCallback cb, void* ctx) {
  if (!path || path[0] == '\0') {
    return Error::INVALID_IMAGE;
  }

  if (!Storage.ready()) {
    LOG_ERR("SD_OTA", "SD card not ready");
    return Error::READ_ERROR;
  }

  HalFile file;
  if (!Storage.openFileForRead("SD_OTA", path, file)) {
    LOG_ERR("SD_OTA", "Failed to open firmware file: %s", path);
    return Error::READ_ERROR;
  }

  const size_t fileSize = file.fileSize();
  const Error result = installer_.installFromFile(file, fileSize, cb, ctx);
  file.close();

  return result;
}
