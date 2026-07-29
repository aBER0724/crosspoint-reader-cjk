#pragma once

#include <cstdint>

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;
  bool tracksRenderGeneration = false;
  uint32_t renderGeneration = 0;

 public:
  // wait=false is for optional background work. It must yield immediately when
  // a page render owns the mutex instead of blocking the input task.
  explicit RenderLock(bool wait = true, bool trackRenderGeneration = false);
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  bool locked() const { return isLocked; }
  uint32_t generation() const { return renderGeneration; }
  bool isStale() const;
  static bool peek();
};
