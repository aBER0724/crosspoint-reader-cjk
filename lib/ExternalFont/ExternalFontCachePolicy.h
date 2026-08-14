#pragma once

#include <cstddef>

namespace ExternalFontCachePolicy {

// 128 entries is the proven safe heap budget on ESP32-C3. Larger caches improve
// occasional hits but leave too little contiguous heap during reader activity.
constexpr size_t kGlyphCacheSize = 128;
constexpr size_t kUiGlyphCacheSize = 64;
constexpr size_t kPreloadLimit = 128;

static_assert(kPreloadLimit <= kGlyphCacheSize, "preload limit must fit in glyph cache");

}  // namespace ExternalFontCachePolicy
