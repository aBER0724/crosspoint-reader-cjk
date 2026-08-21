#include "ExternalFont.h"

const uint8_t* ExternalFont::getGlyph(uint32_t) { return nullptr; }

bool ExternalFont::preloadGlyphs(const uint32_t*, size_t, bool (*)(const void*), const void*) { return true; }

bool ExternalFont::load(const char*) { return true; }

void ExternalFont::unload() {}

ExternalFont::~ExternalFont() = default;

bool ExternalFont::readGlyphFromSD(uint32_t, uint8_t*) { return false; }

bool ExternalFont::parseFilename(const char*) { return true; }

int ExternalFont::findInCache(uint32_t) const { return -1; }

int ExternalFont::getLruSlot() { return 0; }

bool ExternalFont::getGlyphMetrics(uint32_t, uint8_t*, uint8_t*) { return false; }
