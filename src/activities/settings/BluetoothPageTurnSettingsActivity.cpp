#include "BluetoothPageTurnSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BluetoothPageTurnSettingsActivity::onEnter() {
  Activity::onEnter();

  BLUETOOTH_PAGE_TURN.setSettingsSessionActive(true);
  selectedIndex = 0;
  refreshViewModel();
  requestUpdate();
}

void BluetoothPageTurnSettingsActivity::onExit() {
  BLUETOOTH_PAGE_TURN.setSettingsSessionActive(false);
  Activity::onExit();
}

void BluetoothPageTurnSettingsActivity::loop() {
  if (refreshViewModel()) {
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, menuItems.size());
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, menuItems.size());
    requestUpdate();
  });
}

void BluetoothPageTurnSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const std::string statusText = getStatusText();
  const std::string statusDetail = getStatusDetail();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH_PAGE_TURN));
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    statusText.c_str(), statusDetail.empty() ? nullptr : statusDetail.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const Rect contentRect = GUI.getContentRect(renderer, contentTop, metrics.verticalSpacing * 2);
  GUI.drawList(
      renderer, contentRect, static_cast<int>(menuItems.size()), static_cast<int>(selectedIndex),
      [this](int index) { return getItemTitle(index); }, nullptr, nullptr,
      [this](int index) { return getItemValue(index); }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothPageTurnSettingsActivity::rebuildMenuItems() {
  menuItems.clear();
  menuItems.push_back({ItemType::EnabledToggle});
  menuItems.push_back({ItemType::ScanDevices});
  menuItems.push_back({ItemType::BondedDevice});

  if (BLUETOOTH_PAGE_TURN.hasBondedDevice()) {
    menuItems.push_back({ItemType::ForgetBondedDevice});
  }

  const int scannedDeviceCount = BLUETOOTH_PAGE_TURN.getScannedDeviceCount();
  for (int i = 0; i < scannedDeviceCount; ++i) {
    menuItems.push_back({ItemType::ScannedDevice, i});
  }

  if (menuItems.empty()) {
    selectedIndex = 0;
  } else if (selectedIndex >= static_cast<int>(menuItems.size())) {
    selectedIndex = static_cast<int>(menuItems.size()) - 1;
  }
}

bool BluetoothPageTurnSettingsActivity::refreshViewModel() {
  const bool enabled = BLUETOOTH_PAGE_TURN.isEnabled();
  const bool hasBondedDevice = BLUETOOTH_PAGE_TURN.hasBondedDevice();
  const bool connected = BLUETOOTH_PAGE_TURN.isConnected();
  const int scannedDeviceCount = BLUETOOTH_PAGE_TURN.getScannedDeviceCount();
  const auto connectionState = BLUETOOTH_PAGE_TURN.getConnectionState();
  const auto statusReason = BLUETOOTH_PAGE_TURN.getStatusReason();
  const std::string bondedDeviceName = BLUETOOTH_PAGE_TURN.getBondedDeviceName();
  const std::string bondedDeviceAddress = BLUETOOTH_PAGE_TURN.getBondedDeviceAddress();

  const bool hasChanged = enabled != lastEnabled || hasBondedDevice != lastHasBondedDevice ||
                          connected != lastConnected || scannedDeviceCount != lastScannedDeviceCount ||
                          connectionState != lastConnectionState || statusReason != lastStatusReason ||
                          bondedDeviceName != lastBondedDeviceName || bondedDeviceAddress != lastBondedDeviceAddress;

  if (!hasChanged) {
    return false;
  }

  lastEnabled = enabled;
  lastHasBondedDevice = hasBondedDevice;
  lastConnected = connected;
  lastScannedDeviceCount = scannedDeviceCount;
  lastConnectionState = connectionState;
  lastStatusReason = statusReason;
  lastBondedDeviceName = bondedDeviceName;
  lastBondedDeviceAddress = bondedDeviceAddress;
  rebuildMenuItems();
  return true;
}

void BluetoothPageTurnSettingsActivity::handleSelection() {
  if (menuItems.empty()) {
    return;
  }

  const auto item = menuItems[selectedIndex];
  switch (item.type) {
    case ItemType::EnabledToggle:
      BLUETOOTH_PAGE_TURN.setEnabled(!BLUETOOTH_PAGE_TURN.isEnabled());
      break;
    case ItemType::ScanDevices:
      if (BLUETOOTH_PAGE_TURN.isEnabled()) {
        BLUETOOTH_PAGE_TURN.startScan();
      }
      break;
    case ItemType::BondedDevice:
      if (!BLUETOOTH_PAGE_TURN.hasBondedDevice()) {
        return;
      }
      if (!BLUETOOTH_PAGE_TURN.isEnabled()) {
        BLUETOOTH_PAGE_TURN.setEnabled(true);
      }
      if (BLUETOOTH_PAGE_TURN.isConnected()) {
        BLUETOOTH_PAGE_TURN.disconnect();
      } else {
        BLUETOOTH_PAGE_TURN.connectBondedDevice();
      }
      break;
    case ItemType::ForgetBondedDevice:
      BLUETOOTH_PAGE_TURN.forgetBondedDevice();
      break;
    case ItemType::ScannedDevice: {
      if (!BLUETOOTH_PAGE_TURN.isEnabled()) {
        return;
      }
      const auto device = BLUETOOTH_PAGE_TURN.getScannedDevice(item.scannedDeviceIndex);
      if (device.address.empty()) {
        return;
      }
      BLUETOOTH_PAGE_TURN.connectToDevice(device.address, device.name);
      break;
    }
  }

  refreshViewModel();
  requestUpdate();
}

std::string BluetoothPageTurnSettingsActivity::getStatusText() const {
  switch (BLUETOOTH_PAGE_TURN.getConnectionState()) {
    case BluetoothPageTurnManager::ConnectionState::Disabled:
      return tr(STR_STATE_OFF);
    case BluetoothPageTurnManager::ConnectionState::Idle:
      return tr(STR_BLUETOOTH_READY);
    case BluetoothPageTurnManager::ConnectionState::Scanning:
      return tr(STR_SCANNING);
    case BluetoothPageTurnManager::ConnectionState::Connecting:
      return tr(STR_CONNECTING);
    case BluetoothPageTurnManager::ConnectionState::Connected:
      return tr(STR_CONNECTED);
    case BluetoothPageTurnManager::ConnectionState::Error:
      return tr(STR_CONNECTION_FAILED);
  }

  return "";
}

std::string BluetoothPageTurnSettingsActivity::getStatusDetail() const {
  switch (BLUETOOTH_PAGE_TURN.getStatusReason()) {
    case BluetoothPageTurnManager::StatusReason::None:
      break;
    case BluetoothPageTurnManager::StatusReason::StackUnavailable:
      return tr(STR_BLUETOOTH_STACK_UNAVAILABLE);
  }

  const std::string bondedDeviceName = BLUETOOTH_PAGE_TURN.getBondedDeviceName();
  if (!bondedDeviceName.empty()) {
    return bondedDeviceName;
  }

  return BLUETOOTH_PAGE_TURN.getBondedDeviceAddress();
}

std::string BluetoothPageTurnSettingsActivity::getItemTitle(const int index) const {
  if (index < 0 || index >= static_cast<int>(menuItems.size())) {
    return "";
  }

  const auto& item = menuItems[index];
  switch (item.type) {
    case ItemType::EnabledToggle:
      return tr(STR_BLUETOOTH_PAGE_TURN);
    case ItemType::ScanDevices:
      return tr(STR_BLUETOOTH_SCAN_DEVICES);
    case ItemType::BondedDevice:
      return tr(STR_BLUETOOTH_PAIRED_DEVICE);
    case ItemType::ForgetBondedDevice:
      return tr(STR_FORGET_BUTTON);
    case ItemType::ScannedDevice: {
      const auto device = BLUETOOTH_PAGE_TURN.getScannedDevice(item.scannedDeviceIndex);
      if (!device.name.empty()) {
        return device.name;
      }
      if (!device.address.empty()) {
        return device.address;
      }
      return tr(STR_UNNAMED);
    }
  }

  return "";
}

std::string BluetoothPageTurnSettingsActivity::getItemValue(const int index) const {
  if (index < 0 || index >= static_cast<int>(menuItems.size())) {
    return "";
  }

  const auto& item = menuItems[index];
  switch (item.type) {
    case ItemType::EnabledToggle:
      return BLUETOOTH_PAGE_TURN.isEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case ItemType::ScanDevices:
      return BLUETOOTH_PAGE_TURN.getConnectionState() == BluetoothPageTurnManager::ConnectionState::Scanning
                 ? tr(STR_SCANNING)
                 : "";
    case ItemType::BondedDevice:
      if (!BLUETOOTH_PAGE_TURN.hasBondedDevice()) {
        return tr(STR_NOT_SET);
      }
      if (BLUETOOTH_PAGE_TURN.isConnected()) {
        return tr(STR_CONNECTED);
      }
      if (BLUETOOTH_PAGE_TURN.getConnectionState() == BluetoothPageTurnManager::ConnectionState::Connecting) {
        return tr(STR_CONNECTING);
      }
      if (!BLUETOOTH_PAGE_TURN.getBondedDeviceName().empty()) {
        return BLUETOOTH_PAGE_TURN.getBondedDeviceName();
      }
      return BLUETOOTH_PAGE_TURN.getBondedDeviceAddress();
    case ItemType::ForgetBondedDevice:
      return "";
    case ItemType::ScannedDevice: {
      const auto device = BLUETOOTH_PAGE_TURN.getScannedDevice(item.scannedDeviceIndex);
      if (device.address.empty()) {
        return "";
      }
      if (device.address == BLUETOOTH_PAGE_TURN.getBondedDeviceAddress()) {
        return BLUETOOTH_PAGE_TURN.isConnected() ? tr(STR_CONNECTED) : tr(STR_SET);
      }

      char rssiBuffer[8];
      snprintf(rssiBuffer, sizeof(rssiBuffer), "%d", device.rssi);
      return rssiBuffer;
    }
  }

  return "";
}
