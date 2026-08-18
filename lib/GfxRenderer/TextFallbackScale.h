#pragma once

#include <algorithm>
#include <cstdint>

struct TextFallbackScale {
  int numerator = 1;
  int denominator = 1;
};

inline TextFallbackScale makeTextFallbackScale(const int targetAscender, const int targetLineHeight,
                                               const int sourceAscender, const int sourceLineHeight) {
  TextFallbackScale scale;
  const auto tightenScale = [&](const int target, const int source) {
    if (target <= 0 || source <= target) return;
    if (static_cast<int64_t>(target) * scale.denominator < static_cast<int64_t>(scale.numerator) * source) {
      scale.numerator = target;
      scale.denominator = source;
    }
  };
  tightenScale(targetAscender, sourceAscender);
  tightenScale(targetLineHeight, sourceLineHeight);
  return scale;
}

inline TextFallbackScale composeHalfScale(TextFallbackScale scale, const bool halfScale) {
  if (halfScale) scale.denominator *= 2;
  return scale;
}

inline int scaleTextFallbackExtent(const int value, const TextFallbackScale scale) {
  if (value <= 0) return value;
  return std::max(
      1, static_cast<int>((static_cast<int64_t>(value) * scale.numerator + scale.denominator - 1) / scale.denominator));
}

inline int scaleTextFallbackMetric(const int value, const TextFallbackScale scale) {
  const int64_t scaled = static_cast<int64_t>(value) * scale.numerator;
  const int64_t rounding = scale.denominator / 2;
  return static_cast<int>((scaled >= 0 ? scaled + rounding : scaled - rounding) / scale.denominator);
}

inline int textFallbackSourceStart(const int destination, const TextFallbackScale scale) {
  return static_cast<int>(static_cast<int64_t>(destination) * scale.denominator / scale.numerator);
}

inline int textFallbackSourceEnd(const int destination, const int sourceExtent, const TextFallbackScale scale) {
  const int64_t boundary = static_cast<int64_t>(destination + 1) * scale.denominator;
  return std::min(sourceExtent, static_cast<int>((boundary + scale.numerator - 1) / scale.numerator));
}
