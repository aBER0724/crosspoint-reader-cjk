#pragma once

// Rendering runs on a dedicated task while input is processed on the Arduino
// loop task. Page-level cancellation lets a newly requested navigation stop a
// stale render before it monopolizes the render lock.
struct PageRenderCancellation {
  bool (*isCancelled)(const void*) = nullptr;
  const void* context = nullptr;

  bool requested() const { return isCancelled != nullptr && isCancelled(context); }
};
