#include "BluetoothPageTurnManager.h"

#include "CrossPointSettings.h"

namespace {
constexpr const char* kStackUnavailable = "Bluetooth stack unavailable in this build";
}  // namespace

BluetoothPageTurnManager& BluetoothPageTurnManager::getInstance() {
  static BluetoothPageTurnManager instance;
  return instance;
}

void BluetoothPageTurnManager::begin() {
  if (!isEnabled()) {
    setConnectionState(ConnectionState::Disabled);
    return;
  }
  setConnectionState(ConnectionState::Idle, kStackUnavailable);
}

void BluetoothPageTurnManager::update() { state.clearFrameEvents(); }

void BluetoothPageTurnManager::setReaderSessionActive(bool) {}

void BluetoothPageTurnManager::setSettingsSessionActive(bool) {}

void BluetoothPageTurnManager::setEnabled(const bool enabled) {
  SETTINGS.bluetoothPageTurnEnabled = enabled ? 1 : 0;
  SETTINGS.saveToFile();
  if (enabled) {
    setConnectionState(ConnectionState::Idle, kStackUnavailable);
  } else {
    setConnectionState(ConnectionState::Disabled);
  }
}

bool BluetoothPageTurnManager::isEnabled() const { return SETTINGS.bluetoothPageTurnEnabled != 0; }

bool BluetoothPageTurnManager::startScan() {
  if (!isEnabled()) {
    setConnectionState(ConnectionState::Disabled);
    return false;
  }
  setConnectionState(ConnectionState::Error, kStackUnavailable);
  return false;
}

void BluetoothPageTurnManager::stopScan() {
  if (connectionState == ConnectionState::Scanning) {
    setConnectionState(isEnabled() ? ConnectionState::Idle : ConnectionState::Disabled, kStackUnavailable);
  }
}

bool BluetoothPageTurnManager::connectToDevice(const std::string&, const std::string&) {
  if (!isEnabled()) {
    setConnectionState(ConnectionState::Disabled);
    return false;
  }
  setConnectionState(ConnectionState::Error, kStackUnavailable);
  return false;
}

bool BluetoothPageTurnManager::connectBondedDevice() {
  if (!SETTINGS.bluetoothPageTurnBonded || SETTINGS.bluetoothPageTurnAddr[0] == '\0') {
    return false;
  }
  return connectToDevice(SETTINGS.bluetoothPageTurnAddr, SETTINGS.bluetoothPageTurnName);
}

void BluetoothPageTurnManager::disconnect() {
  if (isEnabled()) {
    setConnectionState(ConnectionState::Idle, kStackUnavailable);
  } else {
    setConnectionState(ConnectionState::Disabled);
  }
}

void BluetoothPageTurnManager::forgetBondedDevice() {
  disconnect();
  clearBondedDevice();
}

int BluetoothPageTurnManager::getScannedDeviceCount() const { return 0; }

BluetoothPageTurnManager::ScannedDevice BluetoothPageTurnManager::getScannedDevice(int) const { return {}; }

bool BluetoothPageTurnManager::hasBondedDevice() const { return SETTINGS.bluetoothPageTurnBonded != 0; }

std::string BluetoothPageTurnManager::getBondedDeviceName() const { return SETTINGS.bluetoothPageTurnName; }

std::string BluetoothPageTurnManager::getBondedDeviceAddress() const { return SETTINGS.bluetoothPageTurnAddr; }

bool BluetoothPageTurnManager::isConnected() const { return false; }

BluetoothPageTurnManager::ConnectionState BluetoothPageTurnManager::getConnectionState() const {
  return connectionState;
}

std::string BluetoothPageTurnManager::getStatusMessage() const { return statusMessage; }

void BluetoothPageTurnManager::setConnectionState(const ConnectionState newState, const std::string& message) {
  connectionState = newState;
  statusMessage = message;
}

void BluetoothPageTurnManager::clearBondedDevice() {
  SETTINGS.bluetoothPageTurnBonded = 0;
  SETTINGS.bluetoothPageTurnAddr[0] = '\0';
  SETTINGS.bluetoothPageTurnName[0] = '\0';
  SETTINGS.saveToFile();
}
