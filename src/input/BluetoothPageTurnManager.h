#pragma once

#include <string>

#include "BluetoothPageTurnState.h"

// Compile-time BLE-stack stub. Arduino BLE is lib_ignored while custom_sdkconfig
// omits BT host headers; public API stays stable for MappedInput + settings UI.
class BluetoothPageTurnManager {
 public:
  enum class ConnectionState { Disabled, Idle, Scanning, Connecting, Connected, Error };

  struct ScannedDevice {
    std::string name;
    std::string address;
    int rssi = 0;
    bool hasHidService = false;
  };

  static BluetoothPageTurnManager& getInstance();

  BluetoothPageTurnManager(const BluetoothPageTurnManager&) = delete;
  BluetoothPageTurnManager& operator=(const BluetoothPageTurnManager&) = delete;

  void begin();
  void update();
  void setReaderSessionActive(bool active);
  void setSettingsSessionActive(bool active);

  BluetoothPageTurnState& getState() { return state; }
  const BluetoothPageTurnState& getState() const { return state; }

  void setEnabled(bool enabled);
  bool isEnabled() const;

  bool startScan();
  void stopScan();
  bool connectToDevice(const std::string& address, const std::string& name = "");
  bool connectBondedDevice();
  void disconnect();
  void forgetBondedDevice();

  int getScannedDeviceCount() const;
  ScannedDevice getScannedDevice(int index) const;

  bool hasBondedDevice() const;
  std::string getBondedDeviceName() const;
  std::string getBondedDeviceAddress() const;

  bool isConnected() const;
  ConnectionState getConnectionState() const;
  std::string getStatusMessage() const;

 private:
  BluetoothPageTurnManager() = default;

  void setConnectionState(ConnectionState newState, const std::string& message = "");
  void clearBondedDevice();

  BluetoothPageTurnState state;
  ConnectionState connectionState = ConnectionState::Disabled;
  std::string statusMessage;
};

#define BLUETOOTH_PAGE_TURN BluetoothPageTurnManager::getInstance()
