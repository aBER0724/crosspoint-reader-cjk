#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]

CASES = [
    {
        "name": "FontSelectActivity list items use shared external font label helper",
        "path": ROOT / "src" / "activities" / "settings" / "FontSelectActivity.cpp",
        "required": [
            '#include "util/ExternalFontLabel.h"',
            'return buildExternalFontLabel(info->filename, info->name, info->size, loadable);',
        ],
        "forbidden": [
            'snprintf(label, sizeof(label), "%s (%dpt)%s", info->name, info->size, loadable ? "" : " [!]");',
        ],
    },
    {
        "name": "FontSelectActivity opens preview before applying external fonts",
        "path": ROOT / "src" / "activities" / "settings" / "FontSelectActivity.cpp",
        "required": [
            '#include "activities/util/FontPreviewActivity.h"',
            'std::make_unique<FontPreviewActivity>(',
            'mode == SelectMode::Reader ? FontPreviewActivity::ActionMask::ReaderOnly',
            ': FontPreviewActivity::ActionMask::UiOnly',
        ],
        "forbidden": [
            'void FontSelectActivity::handleSelection()',
        ],
    },
    {
        "name": "FontPreviewActivity exposes source-aware font apply actions",
        "path": ROOT / "src" / "activities" / "util" / "FontPreviewActivity.h",
        "required": [
            'enum class ActionMask { ReaderAndUi, ReaderOnly, UiOnly };',
            'void applyReaderFont();',
            'void applyUiFont();',
            'ActionMask actionMask;',
        ],
        "forbidden": [],
    },
    {
        "name": "FontPreviewActivity does not auto-apply on entry button release",
        "path": ROOT / "src" / "activities" / "util" / "FontPreviewActivity.cpp",
        "required": [
            'mappedInput.wasPressed(MappedInputManager::Button::Confirm)',
            'mappedInput.wasPressed(MappedInputManager::Button::Right)',
        ],
        "forbidden": [
            'mappedInput.wasReleased(MappedInputManager::Button::Confirm)',
            'mappedInput.wasReleased(MappedInputManager::Button::Right)',
        ],
    },
    {
        "name": "FontPreviewActivity avoids exit clear flash",
        "path": ROOT / "src" / "activities" / "util" / "FontPreviewActivity.cpp",
        "required": [
            'void FontPreviewActivity::onExit() { Activity::onExit(); }',
        ],
        "forbidden": [
            'renderer.displayBuffer(HalDisplay::FAST_REFRESH);',
        ],
    },
    {
        "name": "FontPreviewActivity avoids full refresh flicker on entry",
        "path": ROOT / "src" / "activities" / "util" / "FontPreviewActivity.cpp",
        "required": [
            'LOG_DBG("FNTPREV", "displayBuffer FAST_REFRESH...");',
            'renderer.displayBuffer();',
        ],
        "forbidden": [
            'renderer.displayBuffer(HalDisplay::FULL_REFRESH);',
            'displayBuffer FULL_REFRESH',
        ],
    },
    {
        "name": "FontPreviewActivity shows enough sample text",
        "path": ROOT / "src" / "activities" / "util" / "FontPreviewActivity.cpp",
        "required": [
            'const char* SAMPLE_LINES[] = {',
            'for (const char* sampleLine : SAMPLE_LINES)',
            'SAMPLE_LINE_COUNT >= 6',
            r'\xef\xbc\x8c\xe3\x80\x82\xe3\x80\x81\xef\xbc\x9b\xef\xbc\x9a\xef\xbc\x81\xef\xbc\x9f',
        ],
        "forbidden": [
            'SAMPLE_LINE_CJK',
            'SAMPLE_LINE_KANA',
            'SAMPLE_LINE_ASCII',
        ],
    },
    {
        "name": "FontPreviewActivity wraps preview text",
        "path": ROOT / "src" / "activities" / "util" / "FontPreviewActivity.cpp",
        "required": [
            'renderer.wrappedText(previewFontId, sampleLine, contentWidth, remainingLines',
            'renderer.drawText(previewFontId, contentLeft, y, wrappedLine.c_str(), true, style);',
        ],
        "forbidden": [
            'renderer.drawCenteredText(previewFontId, contentTop + static_cast<int>(i) * lineSpacing',
        ],
    },
    {
        "name": "FontPreviewActivity keeps preview font out of page chrome",
        "path": ROOT / "src" / "activities" / "util" / "FontPreviewActivity.cpp",
        "required": [
            'GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, displayName.c_str(),',
            'installPreviewFont();',
            'restorePreviewFont();',
            'LOG_DBG("FNTPREV", "drawButtonHints...");',
            'FontMgr.previewUiFont(previewFontIndex);',
            'FontMgr.previewFont(previewFontIndex);',
        ],
        "forbidden": [
            'installPreviewFont();\n\n  renderer.clearScreen();',
        ],
    },
    {
        "name": "FontManager has non-persistent preview font switching",
        "path": ROOT / "lib" / "ExternalFont" / "FontManager.cpp",
        "required": [
            'bool FontManager::previewFont(int index) {',
            'bool FontManager::previewUiFont(int index) {',
            'restoreFontSelection(int readerIndex, int uiIndex)',
        ],
        "forbidden": [
            'bool FontManager::previewFont(int index) {\n  selectFont(index);',
            'bool FontManager::previewUiFont(int index) {\n  selectUiFont(index);',
        ],
    },
    {
        "name": "FontDownloadActivity frees glyph caches before starting WiFi",
        "path": ROOT / "src" / "activities" / "settings" / "FontDownloadActivity.cpp",
        "required": [
            'FontManager::getInstance().releaseGlyphCaches();',
            'if (auto* cache = renderer.getFontCacheManager()) {',
            'if (!WiFi.mode(WIFI_STA)) {',
            'errorMessage_ = tr(STR_WIFI_CONN_FAILED);',
            'wifiStarted_ = true;',
            'if (wifiStarted_) {\n    // WiFi deinitialization leaves the ESP32-C3 heap too fragmented',
            'silentRestart();',
        ],
        "forbidden": [
            'WiFi.mode(WIFI_STA);\n  startActivityForResult(std::make_unique<WifiSelectionActivity>',
        ],
        "ordered_required": [
            'FontManager::getInstance().releaseGlyphCaches();',
            'if (!WiFi.mode(WIFI_STA)) {',
            'wifiStarted_ = true;',
            'startActivityForResult(std::make_unique<WifiSelectionActivity>',
        ],
    },
    {
        "name": "FontDownloadActivity installs only the previewed point size",
        "path": ROOT / "src" / "activities" / "settings" / "FontDownloadActivity.cpp",
        "required": [
            "downloadFamily(families_[familyIndex], fileIndex, PREVIEW_TMP_PATH);",
            "const bool completeFamily = fileIndex < 0;",
            "const size_t firstFileIndex = completeFamily ? 0 : static_cast<size_t>(fileIndex);",
            "const size_t endFileIndex = completeFamily ? family.files.size() : firstFileIndex + 1;",
            "const bool useStagedFile = !completeFamily && stagedFilePath && Storage.exists(stagedFilePath);",
            "if (!Storage.rename(stagedFilePath, partPath.c_str())) {",
        ],
        "forbidden": [
            "downloadFamily(families_[familyIndex]);",
        ],
    },
    {
        "name": "FontDownloadActivity preserves partial installs during whole-family operations",
        "path": ROOT / "src" / "activities" / "settings" / "FontDownloadActivity.cpp",
        "required": [
            "family.partial = family.installed && (!hasReceipt || !allManifestFilesInstalled);",
            "if (families_[i].installed && !families_[i].partial) continue;",
            "if (!f.installed || f.partial) return true;",
            "if (completeFamily) {\n    std::vector<std::string> retainedFilenames;",
            "fontInstaller_.prepareFamilyPrune(family.name.c_str(), retainedFilenames)",
        ],
        "forbidden": [
            "family.partial = family.installed && !hasReceipt;",
        ],
    },
    {
        "name": "FontInstaller transaction records distinguish complete and partial installs",
        "path": ROOT / "src" / "FontInstaller.cpp",
        "required": [
            "constexpr uint8_t kTransactionRecordVersion1 = 1;",
            "constexpr uint8_t kTransactionRecordVersion2 = 2;",
            "completeFamily = true;",
            "writeTransactionRecord(installingPath, fingerprint, completeFamily)",
            "if (completeFamily) {\n    if (!writeTransactionRecord(receiptTempPath, fingerprint, true)) return false;",
            "readTransactionRecord(receiptPath, installedFingerprint, completeFamily) && completeFamily",
        ],
        "forbidden": [],
    },
    {
        "name": "TextSettingsActivity uses the shared UI and reader size contract",
        "path": ROOT / "src" / "activities" / "settings" / "TextSettingsActivity.cpp",
        "required": [
            "SdCardFontFamilyInfo::UI_POINT_SIZES.begin()",
            "SdCardFontFamilyInfo::UI_POINT_SIZES.end()",
            "for (const uint8_t pointSize : SdCardFontFamilyInfo::UI_POINT_SIZES)",
            "if (font.source == FontEntry::Source::Builtin) {\n    targetCount = 1;",
            "if (!hasUiPointSize(families[font.sourceIndex])) targetCount = 1;",
            "const int selectedTarget = std::min(currentTarget, targetCount - 1);",
            "if (font.source == FontEntry::Source::Builtin && target != FontTarget::Reader) return;",
            "if (targetsUi && !hasUiPointSize(family)) return;",
            'sizes_.push_back({uiPointSizeLabel(families[sdIndex]), 12});',
        ],
        "forbidden": [
            "isUi = isReader && fontManager.getUiSelectedIndex() < 0 && SETTINGS.sdUiFontFamilyName[0] == '\\0';",
            "return isUi ? tr(STR_READER_AND_UI) : tr(STR_EXT_READER_FONT);",
        ],
    },
    {
        "name": "SettingsActivity exposes one unified text settings entry",
        "path": ROOT / "src" / "activities" / "settings" / "SettingsActivity.cpp",
        "required": [
            "if (setting.inTextSettings) continue;",
            "SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings)",
            "std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),",
        ],
        "forbidden": [
            "readerSettings.push_back(SettingInfo::Action(StrId::STR_EXT_READER_FONT",
            "displaySettings.push_back(SettingInfo::Action(StrId::STR_EXT_UI_FONT",
        ],
    },
    {
        "name": "TextSettingsActivity exposes installed reader sizes without scaling CJK fonts",
        "path": ROOT / "src" / "activities" / "settings" / "TextSettingsActivity.cpp",
        "required": [
            'sizes_.push_back({uiPointSizeLabel(families[sdIndex]), 12});',
            "for (uint8_t sizeIndex = 0; sizeIndex < CrossPointSettings::FONT_SIZE_COUNT; ++sizeIndex) {",
            "const SdCardFontFileInfo* file = sdFamily->findClosestReaderSize(sizeIndex);",
            "if (!file ||",
            "std::find(addedPointSizes.begin(), addedPointSizes.end(), file->pointSize) != addedPointSizes.end()",
            'sizes_.push_back({std::to_string(file->pointSize) + " pt", sizeIndex});',
            "const SdCardFontFileInfo* selectedFile = sdFamily->findClosestReaderSize(SETTINGS.fontSize);",
            "Current CJK packs",
            "14/16/18/22 pt",
            "fonts_[currentFamilyIndex_].source != FontEntry::Source::Builtin && sizes_.size() == 1",
            "if (hasFixedExternalSize() || listIndex < 0 || listIndex >= static_cast<int>(sizes_.size())) return;",
        ],
        "forbidden": [
            "const SdCardFontFileInfo* currentFile = sdFamily->findClosestReaderSize(SETTINGS.fontSize);\n    if (currentFile) sizes_.push_back({std::to_string(currentFile->pointSize) + \" pt\", SETTINGS.fontSize});",
        ],
    },
    {
        "name": "TextSettingsActivity preserves reader layout and style controls",
        "path": ROOT / "src" / "activities" / "settings" / "TextSettingsActivity.cpp",
        "required": [
            "case LayoutRow::LineSpacing:",
            "case LayoutRow::FirstLineIndent:",
            "SETTINGS.firstLineIndent = !SETTINGS.firstLineIndent;",
            "case StyleRow::FocusReading:",
            "case StyleRow::Hyphenation:",
            "case StyleRow::EmbeddedStyle:",
            "case StyleRow::AntiAliasing:",
        ],
        "forbidden": [],
    },
    {
        "name": "BaseTheme button hints force BW render mode",
        "path": ROOT / "src" / "components" / "themes" / "BaseTheme.cpp",
        "required": [
            'const auto previousRenderMode = renderer.getRenderMode();',
            'renderer.setRenderMode(GfxRenderer::BW);',
            'renderer.setRenderMode(previousRenderMode);',
        ],
        "forbidden": [],
    },
    {
        "name": "BaseTheme button hints follow color mode",
        "path": ROOT / "src" / "components" / "themes" / "BaseTheme.cpp",
        "required": [
            'renderer.fillRect(stripX, slotY, stripWidth, hintHeight, false);',
            'renderer.drawRect(stripX, slotY, stripWidth, hintHeight, true);',
            'renderer.drawTextRotated90CW(UI_10_FONT_ID, textX, textY, labels[i], true);',
            'renderer.fillRect(x, buttonTop, buttonWidth, buttonHeight, false);',
            'renderer.drawRect(x, buttonTop, buttonWidth, buttonHeight, true);',
            'renderer.drawText(UI_10_FONT_ID, textX, buttonTop + textYOffset, labels[i], true);',
        ],
        "forbidden": [
            'renderer.fillRect(stripX, slotY, stripWidth, hintHeight, true);',
            'renderer.fillRect(x, buttonTop, buttonWidth, buttonHeight, true);',
        ],
    },
    {
        "name": "LyraTheme button hints force BW render mode",
        "path": ROOT / "src" / "components" / "themes" / "lyra" / "LyraTheme.cpp",
        "required": [
            'const auto previousRenderMode = renderer.getRenderMode();',
            'renderer.setRenderMode(GfxRenderer::BW);',
            'renderer.setRenderMode(previousRenderMode);',
        ],
        "forbidden": [],
    },
    {
        "name": "LyraTheme button hints follow color mode",
        "path": ROOT / "src" / "components" / "themes" / "lyra" / "LyraTheme.cpp",
        "required": [
            'renderer.fillRoundedRect(stripX, slotY, stripWidth, hintHeight, cornerRadius, Color::White);',
            'renderer.drawRoundedRect(stripX, slotY, stripWidth, hintHeight, 1, cornerRadius, /*topLeft=*/roundLeft,',
            '/*topRight=*/roundRight, /*bottomLeft=*/roundLeft, /*bottomRight=*/roundRight, true);',
            'renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i], true);',
            'renderer.fillRoundedRect(x, buttonTop, buttonWidth, buttonHeight, cornerRadius, Color::White);',
            'renderer.drawRoundedRect(x, buttonTop, buttonWidth, buttonHeight, 1, cornerRadius, roundTop, roundTop,',
            'roundBottom, roundBottom, true);',
            'renderer.drawText(SMALL_FONT_ID, textX, buttonTop + textYOffset, labels[i], true);',
            'renderer.fillRoundedRect(x, smallButtonTop, buttonWidth, smallButtonHeight, cornerRadius, Color::White);',
        ],
        "forbidden": [
            'renderer.fillRoundedRect(stripX, slotY, stripWidth, hintHeight, cornerRadius, Color::Black);',
            'renderer.fillRoundedRect(x, buttonTop, buttonWidth, buttonHeight, cornerRadius, Color::Black);',
            'renderer.fillRoundedRect(x, smallButtonTop, buttonWidth, smallButtonHeight, cornerRadius, Color::Black);',
        ],
    },
    {
        "name": "GfxRenderer always redrives dark-mode FAST/HALF UI",
        "path": ROOT / "lib" / "GfxRenderer" / "GfxRenderer.cpp",
        "required": [
            'if (darkMode &&\n      (effectiveRefreshMode == HalDisplay::FAST_REFRESH || effectiveRefreshMode == HalDisplay::HALF_REFRESH)) {',
            'display.displayBuffer(HalDisplay::DARK_REDRIVE, fadingFix);',
        ],
        "forbidden": [
            'if (!darkUiRedrivePrimed) {',
            'darkUiFastRefreshesSinceRedrive++',
            'darkUiFastRefreshesSinceRedrive >= DARK_UI_FAST_REFRESHES_PER_REDRIVE',
        ],
    },
    {
        "name": "GfxRenderer applies byte-aligned UI partial update rectangles",
        "path": ROOT / "lib" / "GfxRenderer" / "GfxRenderer.cpp",
        "required": [
            'const bool hasPartialUpdate = partialW_ > 0 && partialH_ > 0;',
            'logicalRectToPhysicalWindow(orientation, partialX_, partialY_, partialW_, partialH_, getScreenWidth(),',
            'partialX_ = partialY_ = partialW_ = partialH_ = 0;',
            'display.displayWindowDarkRedrive(physicalWindow.x, physicalWindow.y, physicalWindow.width,',
            'display.displayWindow(physicalWindow.x, physicalWindow.y, physicalWindow.width, physicalWindow.height,',
            'display.displayBuffer(effectiveRefreshMode, fadingFix);',
        ],
        "forbidden": [],
    },
    {
        "name": "GfxRenderer resets dark UI redrive throttle when dark mode changes",
        "path": ROOT / "lib" / "GfxRenderer" / "GfxRenderer.h",
        "required": [
            'mutable bool darkUiRedrivePrimed = false;',
            'mutable unsigned long lastDarkUiRedriveMs = 0;',
            'mutable uint8_t darkUiFastRefreshesSinceRedrive = 0;',
            'darkUiRedrivePrimed = false;',
            'darkUiFastRefreshesSinceRedrive = 0;',
        ],
        "forbidden": [],
    },
    {
        "name": "HomeActivity uses partial refresh for menu and supported cover selection moves",
        "path": ROOT / "src" / "activities" / "home" / "HomeActivity.cpp",
        "required": [
            "const bool menuOnlyPartialUpdate = canUseMenuOnlyPartialUpdate(lastRenderedSelectorIndex, renderedSelectorIndex);",
            "const Rect dirtyRect = GUI.getHomeMenuDirtyRect(menuRect, previousMenuIndex, currentMenuIndex);",
            "renderer.setPartialUpdateRect(dirtyRect.x, dirtyRect.y, dirtyRect.width, dirtyRect.height);",
            "bool coverOnlyPartialUpdate = canUseCoverOnlyPartialUpdate(lastRenderedSelectorIndex, renderedSelectorIndex);",
            "bufferRestored = coverBufferStored && restoreCoverBuffer();",
            "GUI.drawHomeCoverSelectionUpdate(",
            "if (!menuOnlyPartialUpdate && !coverOnlyPartialUpdate) {",
            "if (menuOnlyPartialUpdate || coverOnlyPartialUpdate) {",
            "} else if (!firstRenderDone) {",
            "renderer.waitRefreshComplete();",
            "const int renderedSelectorIndex = selectorIndex;",
            "lastRenderedSelectorIndex = renderedSelectorIndex;",
            "GUI.drawRecentBookCover(\n        renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, *booksForCover,",
            "std::array<const char*, 6> menuItems{};",
            "GUI.drawButtonMenuRange(renderer, menuRect, menuItemCount, selectedMenuIndex, menuLabel, menuIcon,",
            "GUI.supportsHomeMenuRowUpdates()",
            "bool HomeActivity::canUseMenuOnlyPartialUpdate",
            "bool HomeActivity::canUseCoverOnlyPartialUpdate",
        ],
        "forbidden": [
            "setMenuPartialUpdateIfSafe(",
        ],
    },
    {
        "name": "HomeActivity tracks full redraw and last rendered selection for partial menu path",
        "path": ROOT / "src" / "activities" / "home" / "HomeActivity.h",
        "required": [
            "bool coverBufferDarkMode = false;",
            "bool fullRedrawRequired = true;",
            "int lastRenderedSelectorIndex = -1;",
            "bool canUseMenuOnlyPartialUpdate(int fromIndex, int toIndex) const;",
            "bool canUseCoverOnlyPartialUpdate(int fromIndex, int toIndex) const;",
        ],
        "forbidden": [
            "void setMenuPartialUpdateIfSafe(int oldIndex, int newIndex);",
        ],
    },
    {
        "name": "HomeActivity invalidates cached cover framebuffer across color-mode changes",
        "path": ROOT / "src" / "activities" / "home" / "HomeActivity.cpp",
        "required": [
            'coverBufferDarkMode = renderer.isDarkMode();',
            'if (coverBufferDarkMode != renderer.isDarkMode()) {',
            'freeCoverBuffer();',
            'coverRendered = false;',
        ],
        "forbidden": [],
    },
    {
        "name": "LyraTheme stores cover cache before selected-state overlay",
        "path": ROOT / "src" / "components" / "themes" / "lyra" / "LyraTheme.cpp",
        "required": [
            'renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,',
            'coverBufferStored = storeCoverBuffer();',
            'coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer',
            'bool bookSelected = (selectorIndex == 0);',
            'if (bookSelected) {',
        ],
        "forbidden": [],
        "ordered_required": [
            'renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,',
            'coverBufferStored = storeCoverBuffer();',
            'coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer',
            'bool bookSelected = (selectorIndex == 0);',
            'if (bookSelected) {',
        ],
    },
    {
        "name": "Lyra3CoversTheme stores an unselected baseline and restores cached selection tiles",
        "path": ROOT / "src" / "components" / "themes" / "lyra" / "Lyra3CoversTheme.cpp",
        "required": [
            'renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,',
            'drawCoverTitle(renderer, tileX, tileY, title);',
            'coverBufferStored = storeCoverBuffer();',
            'coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer',
            'buildSelectionBuffers(renderer, rect, recentBooks);',
            'restoreSelectionBuffer(renderer, selectorIndex)',
            'Rect Lyra3CoversTheme::drawHomeCoverSelectionUpdate',
        ],
        "forbidden": [],
        "ordered_required": [
            'renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,',
            'drawCoverTitle(renderer, tileX, tileY, title);',
            'coverBufferStored = storeCoverBuffer();',
            'coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer',
            'buildSelectionBuffers(renderer, rect, recentBooks);',
        ],
    },
    {
        "name": "Epub thumbnail cache path is versioned",
        "path": ROOT / "lib" / "Epub" / "Epub.cpp",
        "required": [
            'constexpr int THUMB_BMP_CACHE_VERSION = 2;',
            'return cachePath + "/thumb_v" + std::to_string(THUMB_BMP_CACHE_VERSION) + "_" + std::to_string(height) + ".bmp";',
        ],
        "forbidden": [
            'return cachePath + "/thumb_" + std::to_string(height) + ".bmp";',
        ],
    },
    {
        "name": "Xtc thumbnail cache path is versioned",
        "path": ROOT / "lib" / "Xtc" / "Xtc.cpp",
        "required": [
            'constexpr int THUMB_BMP_CACHE_VERSION = 2;',
            'return cachePath + "/thumb_v" + std::to_string(THUMB_BMP_CACHE_VERSION) + "_" + std::to_string(height) + ".bmp";',
        ],
        "forbidden": [
            'return cachePath + "/thumb_" + std::to_string(height) + ".bmp";',
        ],
    },
    {
        "name": "FreeInkDisplay window refresh writes framebuffer rows without temporary copies",
        "path": ROOT / "freeink-sdk" / "libs" / "display" / "FreeInkDisplay" / "src" / "driver" / "Ssd1677Driver.cpp",
        "required": [
            'writeWindowRam(bus, CMD_WRITE_RAM_BW, fb, x, y, w, h, /*invert=*/false);',
            'writeWindowRam(bus, CMD_WRITE_RAM_RED, prev, x, y, w, h, /*invert=*/false);',
            'writeWindowRam(bus, CMD_WRITE_RAM_RED, fb, x, y, w, h, /*invert=*/false);',
        ],
        "forbidden": ["windowBuffer", "previousWindow"],
    },
    {
        "name": "FreeInkDisplay window refresh preserves the async full-frame baseline",
        "path": ROOT / "freeink-sdk" / "libs" / "display" / "FreeInkDisplay" / "src" / "FreeInkDisplay.cpp",
        "required": [
            "_driver->displayWindow(_bus, frameBuffer, _shadowValid ? _asyncShadow : nullptr, x, y, w, h, turnOffScreen);",
            "syncWindowToActive(_asyncShadow, frameBuffer, displayWidthBytes, x, y, w, h);",
            "if (_shadowValid && _asyncShadow != nullptr) _driver->seedPreviousFrame(_bus, _asyncShadow);",
            "Move its displayed-frame baseline into controller RAM first",
        ],
        "forbidden": [
            "_driver->displayWindow(_bus, frameBuffer, nullptr, x, y, w, h, turnOffScreen);",
        ],
    },
    {
        "name": "ReaderUtils still owns dark-mode reader redrive",
        "path": ROOT / "src" / "activities" / "reader" / "ReaderUtils.h",
        "required": [
            'if (renderer.isDarkMode() || decision.mode == ReaderRuntime::RefreshMode::DarkRedrive) {',
            'renderer.displayBufferDarkRedrive();',
        ],
        "forbidden": [
            'case ReaderRuntime::RefreshMode::DarkRedrive:',
        ],
    },
    {
        "name": "CrossPointWebServer options use shared external font label helper",
        "path": ROOT / "src" / "network" / "CrossPointWebServer.cpp",
        "required": [
            '#include "util/ExternalFontLabel.h"',
            'std::string label = buildExternalFontLabel(info->filename, info->name, info->size,',
        ],
        "forbidden": [
            'std::string label = std::string(info->name) + " (" + std::to_string(info->size) + "pt)";',
        ],
    },
]


def main() -> int:
    failures: list[str] = []

    for case in CASES:
        content = case["path"].read_text(encoding="utf-8")
        for needle in case["required"]:
            if needle not in content:
                failures.append(f"{case['name']}: missing required snippet: {needle}")
        for needle in case["forbidden"]:
            if needle in content:
                failures.append(f"{case['name']}: forbidden snippet still present: {needle}")

    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("EXTERNAL_FONT_DISPLAY_PATHS_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
