#pragma once

#include <cstdint>

namespace FrontButtonOrientation {

enum class Orientation : uint8_t { Portrait, PortraitInverted, LandscapeClockwise, LandscapeCounterClockwise };

constexpr uint8_t mirrorFrontIndex(const uint8_t hardwareIndex) {
  return hardwareIndex < 4 ? static_cast<uint8_t>(3 - hardwareIndex) : hardwareIndex;
}

constexpr bool isInvertedPortrait(const Orientation orientation) {
  return orientation == Orientation::PortraitInverted;
}

constexpr uint8_t inputHardwareIndex(const bool followOrientation, const Orientation orientation,
                                     const uint8_t configuredHardwareIndex) {
  return followOrientation && isInvertedPortrait(orientation) ? mirrorFrontIndex(configuredHardwareIndex)
                                                              : configuredHardwareIndex;
}

constexpr uint8_t hintHardwareIndex(const bool followOrientation, const Orientation orientation,
                                    const uint8_t screenSlotIndex) {
  return !followOrientation && isInvertedPortrait(orientation) ? mirrorFrontIndex(screenSlotIndex) : screenSlotIndex;
}

constexpr bool navigationSwapped(const bool followOrientation, const Orientation orientation) {
  // Inverted portrait already mirrors the configured front roles. Swapping previous/next here would reverse
  // Left/Right a second time. Landscape CCW still needs the existing semantic axis swap.
  return followOrientation && orientation == Orientation::LandscapeCounterClockwise;
}

}  // namespace FrontButtonOrientation
