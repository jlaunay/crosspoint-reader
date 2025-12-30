#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <vector>

#include "activities/Activity.h"
#include "BluetoothPageTurner.h"

enum class BluetoothSettingsState {
  MENU,          // Show main menu
  SCANNING,      // Scanning for devices
  DEVICE_LIST,   // Show list of found devices
  CONNECTING,    // Connecting to selected device
};

class BluetoothSettingsActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  BluetoothSettingsState state = BluetoothSettingsState::MENU;
  int selectedOption = 0;
  bool bluetoothEnabled = false;
  const std::function<void()> onGoBack;

  // Scan results
  std::vector<BluetoothPageTurner::ScannedDevice> devices;
  int selectedDeviceIndex = 0;
  std::string connectingToAddress;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderMenu() const;
  void renderScanning() const;
  void renderDeviceList() const;
  void renderConnecting() const;

  void enableBluetooth();
  void disableBluetooth();
  void startScan();
  void connectToSelectedDevice();
  void disconnectDevice();
  void clearBluetoothCache();
  int getMenuItemCount() const;

public:
  explicit BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    const std::function<void()>& onGoBack)
      : Activity("BluetoothSettings", renderer, mappedInput), onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};