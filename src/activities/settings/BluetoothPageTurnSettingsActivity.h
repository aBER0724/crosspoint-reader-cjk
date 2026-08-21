#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "../ActivityResult.h"
#include "input/BluetoothPageTurnManager.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

class BluetoothPageTurnSettingsActivity final : public Activity {
 public:
  explicit BluetoothPageTurnSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BluetoothPageTurnSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class ItemType { EnabledToggle, ScanDevices, BondedDevice, ForgetBondedDevice, ScannedDevice };

  struct MenuItem {
    ItemType type;
    int scannedDeviceIndex = -1;
  };

  void rebuildMenuItems();
  bool refreshViewModel();
  void handleSelection();
  std::string getStatusText() const;
  std::string getStatusDetail() const;
  std::string getItemTitle(int index) const;
  std::string getItemValue(int index) const;

  ButtonNavigator buttonNavigator;
  std::vector<MenuItem> menuItems;
  int selectedIndex = 0;

  bool lastEnabled = false;
  bool lastHasBondedDevice = false;
  bool lastConnected = false;
  int lastScannedDeviceCount = -1;
  BluetoothPageTurnManager::ConnectionState lastConnectionState = BluetoothPageTurnManager::ConnectionState::Disabled;
  BluetoothPageTurnManager::StatusReason lastStatusReason = BluetoothPageTurnManager::StatusReason::None;
  std::string lastBondedDeviceName;
  std::string lastBondedDeviceAddress;
};
