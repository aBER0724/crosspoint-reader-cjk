#pragma once

#include <Epub.h>
#include <Logging.h>

#include <mutex>
#include <optional>
#include <string>

#include "ProgressFile.h"

namespace EpubReaderUtils {

// Persists reader progress to an EPUB cache directory. Returns true on success.
inline bool saveProgress(const std::string& cachePath, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  if (!ProgressFile::writeAtomic(cachePath, data, sizeof(data))) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

// Convenience overload for callers that still own the EPUB instance.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  return saveProgress(epub.getCachePath(), spineIndex, pageNumber, pageCount);
}

struct DeferredProgress {
  std::string cachePath;
  int spineIndex;
  int pageNumber;
  int pageCount;

  bool matches(const DeferredProgress& other) const {
    return cachePath == other.cachePath && spineIndex == other.spineIndex && pageNumber == other.pageNumber &&
           pageCount == other.pageCount;
  }
};

inline std::optional<DeferredProgress>& queuedProgress() {
  static std::optional<DeferredProgress> progress;
  return progress;
}

inline std::optional<DeferredProgress>& savedProgress() {
  static std::optional<DeferredProgress> progress;
  return progress;
}

inline std::mutex& deferredProgressMutex() {
  static std::mutex mutex;
  return mutex;
}

// Queue the most recent reading position. ActivityManager flushes it only
// after the UI is idle, so the atomic FAT replacement never blocks input.
inline void queueProgressSave(std::string cachePath, int spineIndex, int pageNumber, int pageCount) {
  std::lock_guard<std::mutex> lock(deferredProgressMutex());
  DeferredProgress progress{std::move(cachePath), spineIndex, pageNumber, pageCount};
  if (savedProgress() && savedProgress()->matches(progress)) {
    return;
  }
  if (!queuedProgress() || !queuedProgress()->matches(progress)) {
    queuedProgress() = std::move(progress);
  }
}

inline bool hasQueuedProgressSave() {
  std::lock_guard<std::mutex> lock(deferredProgressMutex());
  return queuedProgress().has_value();
}

inline bool flushQueuedProgressSave() {
  DeferredProgress progress;
  {
    std::lock_guard<std::mutex> lock(deferredProgressMutex());
    if (!queuedProgress()) {
      return true;
    }
    progress = *queuedProgress();
  }
  if (!saveProgress(progress.cachePath, progress.spineIndex, progress.pageNumber, progress.pageCount)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(deferredProgressMutex());
    savedProgress() = progress;
    if (queuedProgress() && queuedProgress()->matches(progress)) {
      queuedProgress().reset();
    }
  }
  return true;
}

// A synchronous caller has already written this book's current state, or is
// about to remove its cache. Forget a matching deferred snapshot so it cannot
// overwrite the canonical progress file later.
inline void discardQueuedProgressSave(const std::string& cachePath) {
  std::lock_guard<std::mutex> lock(deferredProgressMutex());
  if (queuedProgress() && queuedProgress()->cachePath == cachePath) {
    queuedProgress().reset();
  }
  if (savedProgress() && savedProgress()->cachePath == cachePath) {
    savedProgress().reset();
  }
}

// A finished book can be moved together with its cache. Keep an unsaved
// position pointed at the renamed cache so its later idle flush remains valid.
inline void repointQueuedProgressSave(const std::string& oldCachePath, const std::string& newCachePath) {
  std::lock_guard<std::mutex> lock(deferredProgressMutex());
  if (queuedProgress() && queuedProgress()->cachePath == oldCachePath) {
    queuedProgress()->cachePath = newCachePath;
  }
  if (savedProgress() && savedProgress()->cachePath == oldCachePath) {
    savedProgress()->cachePath = newCachePath;
  }
}

}  // namespace EpubReaderUtils
