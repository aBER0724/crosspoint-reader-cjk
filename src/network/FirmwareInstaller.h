#pragma once

#include <HalStorage.h>

#include <cstddef>

class FirmwareInstaller {
 public:
  enum class Error {
    OK = 0,
    NO_PARTITION,
    IMAGE_TOO_LARGE,
    INVALID_IMAGE,
    READ_ERROR,
    OTA_BEGIN_FAILED,
    OTA_WRITE_FAILED,
    OTA_END_FAILED,
  };

  using ProgressCallback = void (*)(size_t processed, size_t total, void* ctx);

  static constexpr size_t kMaxAppImageSize = 0x680000;

  static Error validateImageHeader(HalFile& file);

  Error installFromFile(HalFile& file, size_t fileSize, ProgressCallback cb, void* ctx);

  size_t getProcessedSize() const { return processedBytes; }
  size_t getTotalSize() const { return totalBytes; }

 private:
  static constexpr size_t kReadBufferSize = 4096;

  size_t processedBytes = 0;
  size_t totalBytes = 0;
};
