#include "ReaderRuntimePolicy.h"

#include <algorithm>

namespace ReaderRuntime {
namespace {

uint16_t normalizedFrequency(const uint16_t refreshFrequency) { return std::max<uint16_t>(refreshFrequency, 1); }

uint16_t decrementCadence(const uint16_t cadenceRemaining) {
  if (cadenceRemaining <= 1) {
    return 0;
  }
  return static_cast<uint16_t>(cadenceRemaining - 1);
}

bool needsStateResync(const RefreshContext& context) {
  return context.settingsChanged || context.wakeResume || context.cacheRebuilt;
}

}  // namespace

MemoryDecision classifyReaderMemory(const uint32_t freeHeap, const uint32_t requiredHeap) {
  if (freeHeap < MemoryThresholds::sectionBuildStopHeap) {
    return MemoryDecision::Stop;
  }
  if (freeHeap < MemoryThresholds::optionalQualityHeap) {
    return MemoryDecision::Degrade;
  }
  if (requiredHeap > 0 && freeHeap < requiredHeap) {
    return MemoryDecision::Degrade;
  }
  return MemoryDecision::Proceed;
}

RefreshDecision chooseReaderRefresh(const RefreshContext& context) {
  const uint16_t frequency = normalizedFrequency(context.refreshFrequency);
  const uint16_t cadenceRemaining = context.cadenceRemaining == 0 ? frequency : context.cadenceRemaining;

  RefreshDecision decision{};
  decision.nextCadenceRemaining = decrementCadence(cadenceRemaining);
  const bool quickConsecutiveTurn = context.consecutiveTurns > 1;
  decision.runGrayscalePass = context.textAntiAliasing && context.grayscaleRequested && context.containsImages &&
                              !context.externalFontEnabled && !context.lowMemory && !quickConsecutiveTurn;

  if (context.chapterBoundary) {
    decision.mode = context.darkMode ? RefreshMode::DarkRedrive : RefreshMode::Half;
    decision.reason = RefreshReason::ChapterBoundary;
    decision.nextCadenceRemaining = frequency;
    return decision;
  }

  if (needsStateResync(context)) {
    decision.mode = context.darkMode ? RefreshMode::DarkRedrive : RefreshMode::Half;
    decision.reason = RefreshReason::StateResync;
    decision.nextCadenceRemaining = frequency;
    return decision;
  }

  if (context.lowMemory) {
    decision.mode = context.darkMode ? RefreshMode::DarkRedrive : RefreshMode::Fast;
    decision.reason = RefreshReason::LowMemoryDegrade;
    decision.useImageDoubleFast = false;
    decision.runGrayscalePass = false;
    return decision;
  }

  if (context.containsImages && context.textAntiAliasing) {
    decision.mode = context.darkMode ? RefreshMode::DarkRedrive : RefreshMode::Fast;
    decision.reason = RefreshReason::ImageDoubleFast;
    decision.useImageDoubleFast = true;
    decision.nextCadenceRemaining = cadenceRemaining;
    return decision;
  }

  if (context.consecutiveTurns >= frequency + 1) {
    decision.mode = context.darkMode ? RefreshMode::DarkRedrive : RefreshMode::Half;
    decision.reason = RefreshReason::ContinuousTurns;
    decision.nextCadenceRemaining = frequency;
    return decision;
  }

  if (cadenceRemaining <= 1) {
    decision.mode = context.darkMode ? RefreshMode::DarkRedrive : RefreshMode::Half;
    decision.reason = RefreshReason::Cadence;
    decision.nextCadenceRemaining = frequency;
    return decision;
  }

  if (context.darkMode) {
    decision.mode = RefreshMode::DarkRedrive;
    decision.reason = RefreshReason::DarkMode;
    return decision;
  }

  decision.mode = RefreshMode::Fast;
  decision.reason = RefreshReason::NormalText;
  return decision;
}

}  // namespace ReaderRuntime
