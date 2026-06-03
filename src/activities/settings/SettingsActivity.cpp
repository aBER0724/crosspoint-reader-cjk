#include "SettingsActivity.h"

#include <FontManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdio>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "FontSelectActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "LineSpacingSelectionActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ExternalFontLabel.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Build per-category vectors from the shared settings list
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  for (const auto& setting : getSettingsList()) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
    // Web-only categories (KOReader Sync, OPDS Browser) are skipped for device UI
  }

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_BROWSER, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_INSTALL_FIRMWARE_SD, SettingAction::InstallFirmwareFromSd));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  displaySettings.push_back(SettingInfo::Action(StrId::STR_EXT_UI_FONT, SettingAction::SelectUiFont));

  // Initialize selection based on caller hint.
  if (initialCategoryIndex < 0 || initialCategoryIndex >= categoryCount) {
    selectedCategoryIndex = 0;
  } else {
    selectedCategoryIndex = initialCategoryIndex;
  }

  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
    default:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());

  if (initialSettingIndex < 0) {
    selectedSettingIndex = 0;
  } else if (initialSettingIndex > settingsCount) {
    selectedSettingIndex = settingsCount;
  } else {
    selectedSettingIndex = initialSettingIndex;
  }

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    finish();
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    switch (selectedCategoryIndex) {
      case 0:
        currentSettings = &displaySettings;
        break;
      case 1:
        currentSettings = &readerSettings;
        break;
      case 2:
        currentSettings = &controlsSettings;
        break;
      case 3:
        currentSettings = &systemSettings;
        break;
    }
    settingsCount = static_cast<int>(currentSettings->size());
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
    // Apply invert images change immediately
    if (setting.nameId == StrId::STR_INVERT_IMAGES) {
      renderer.setInvertImagesInDarkMode(SETTINGS.invertImages);
    }
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Font Size: skip when external font is selected (fixed bitmap size)
    if (setting.nameId == StrId::STR_FONT_SIZE && FontMgr.getSelectedIndex() >= 0) {
      return;
    }
    // Font Family: open FontSelectActivity (combined built-in + external fonts)
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      startActivityForResult(
          std::make_unique<FontSelectActivity>(renderer, mappedInput, FontSelectActivity::SelectMode::Reader),
          [this](const ActivityResult&) {
            invalidateSectionPreservingPosition();
            requestUpdate();
          });
      return;
    }
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());

    // Apply dark mode change immediately (renderer needs explicit notification)
    if (setting.nameId == StrId::STR_COLOR_MODE) {
      renderer.setDarkMode(SETTINGS.colorMode == CrossPointSettings::COLOR_MODE::DARK_MODE);
      forceFullSettingsRefresh = true;
    }
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // Line spacing uses a slider activity (0.8x-2.5x) for finer control.
    if (setting.nameId == StrId::STR_LINE_SPACING) {
      startActivityForResult(
          std::make_unique<LineSpacingSelectionActivity>(renderer, mappedInput, static_cast<int>(SETTINGS.lineSpacing)),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              auto* data = std::get_if<PercentResult>(&result.data);
              if (data) {
                SETTINGS.lineSpacing = static_cast<uint8_t>(data->percent);
                SETTINGS.saveToFile();
              }
            }
            requestUpdate();
          });
      return;
    }

    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<CalibreConnectActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::InstallFirmwareFromSd:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::SelectUiFont:
        startActivityForResult(
            std::make_unique<FontSelectActivity>(renderer, mappedInput, FontSelectActivity::SelectMode::UI),
            [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    // ACTION types don't change settings values, skip saveToFile().
    // Also avoids concurrent SD card access with the newly created render task.
    return;
  } else {
    return;
  }

  SETTINGS.saveToFile();
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding + hintGutterHeight, pageWidth, metrics.headerHeight},
                 tr(STR_SETTINGS_TITLE), CROSSPOINT_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer,
                 Rect{0, metrics.topPadding + hintGutterHeight + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                 tabs, selectedSettingIndex == 0);

  const auto& settings = *currentSettings;
  const int settingsListTop =
      metrics.topPadding + hintGutterHeight + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  GUI.drawList(
      renderer, GUI.getContentRect(renderer, settingsListTop, metrics.verticalSpacing * 2), settingsCount,
      selectedSettingIndex - 1, [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); },
      nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          // Font Family: show external font name when selected
          if (setting.nameId == StrId::STR_FONT_FAMILY && FontMgr.getSelectedIndex() >= 0) {
            const FontInfo* info = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
            valueText = info ? buildExternalFontLabel(info->filename, info->name, info->size,
                                                      ExternalFont::canFitGlyph(info->width, info->height))
                             : tr(STR_EXTERNAL_FONT);
            // Font Size: show actual pixel size when external font is active
          } else if (setting.nameId == StrId::STR_FONT_SIZE && FontMgr.getSelectedIndex() >= 0) {
            const FontInfo* info = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
            valueText = info ? (std::to_string(info->size) + "pt") : "—";
          } else {
            const uint8_t value = SETTINGS.*(setting.valuePtr);
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_LINE_SPACING) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2fx", static_cast<float>(SETTINGS.*(setting.valuePtr)) / 100.0f);
            valueText = buf;
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        } else if (setting.type == SettingType::ACTION && setting.nameId == StrId::STR_EXT_UI_FONT) {
          // Show current UI font name or "Built-in"
          if (FontMgr.isUiFontEnabled()) {
            const int idx = FontMgr.getUiSelectedIndex();
            const FontInfo* info = FontMgr.getFontInfo(idx);
            valueText = info ? buildExternalFontLabel(info->filename, info->name, info->size,
                                                      ExternalFont::canFitGlyph(info->width, info->height))
                             : tr(STR_EXTERNAL_FONT);
          } else {
            valueText = tr(STR_BUILTIN_DISABLED);
          }
        }
        return valueText;
      },
      true);

  // Draw help text
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (forceFullSettingsRefresh) {
    forceFullSettingsRefresh = false;
    if (renderer.isDarkMode()) {
      renderer.displayBufferDarkRedrive();
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  } else if (renderer.isDarkMode()) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBuffer();
  }
}

void SettingsActivity::invalidateSectionPreservingPosition() {
  // No-op in standalone Settings; only meaningful when pushed from EpubReaderActivity.
}