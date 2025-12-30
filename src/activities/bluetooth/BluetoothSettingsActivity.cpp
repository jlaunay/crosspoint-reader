#include "BluetoothSettingsActivity.h"

#include <GfxRenderer.h>
#include "MappedInputManager.h"
#include "fontIds.h"

void BluetoothSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<BluetoothSettingsActivity*>(param);
  self->displayTaskLoop();
}

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Check if Bluetooth is already initialized
  bluetoothEnabled = BT_PAGE_TURNER.isInitialized();
  state = BluetoothSettingsState::MENU;

  selectedOption = 0;
  selectedDeviceIndex = 0;
  updateRequired = true;

  xTaskCreate(&BluetoothSettingsActivity::taskTrampoline, "BluetoothSettingsTask",
              4096,               // Stack size (increased for BLE operations)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void BluetoothSettingsActivity::onExit() {
  Activity::onExit();

  // Stop continuous scan if active
  if (BT_PAGE_TURNER.isContinuousScanning()) {
    BT_PAGE_TURNER.stopContinuousScan();
  }

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

int BluetoothSettingsActivity::getMenuItemCount() const {
  if (!bluetoothEnabled) {
    return 2; // Enable, Back
  }

  if (BT_PAGE_TURNER.isConnected()) {
    return 5; // Disconnect, Disable, Scan, Clear Cache, Back
  }

  return 4; // Disable, Scan, Clear Cache, Back
}

void BluetoothSettingsActivity::loop() {
  // Update state based on connection status changes
  if (bluetoothEnabled && state == BluetoothSettingsState::CONNECTING) {
    if (BT_PAGE_TURNER.isConnected()) {
      state = BluetoothSettingsState::MENU;
      selectedOption = 0;
      updateRequired = true;
    }
  }

  // Check for connection status changes in menu
  static bool wasConnected = false;
  if (state == BluetoothSettingsState::MENU && bluetoothEnabled) {
    bool nowConnected = BT_PAGE_TURNER.isConnected();
    if (wasConnected != nowConnected) {
      wasConnected = nowConnected;
      updateRequired = true;
    }
  }

  if (state == BluetoothSettingsState::MENU) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoBack();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Up) || mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (selectedOption > 0) {
        selectedOption--;
        updateRequired = true;
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) || mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      const int maxOption = getMenuItemCount() - 1;
      if (selectedOption < maxOption) {
        selectedOption++;
        updateRequired = true;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!bluetoothEnabled) {
        // Menu: Enable, Back
        if (selectedOption == 0) {
          enableBluetooth();
        } else {
          onGoBack();
        }
      } else if (BT_PAGE_TURNER.isConnected()) {
        // Menu: Disconnect, Disable, Scan, Clear Cache, Back
        if (selectedOption == 0) {
          disconnectDevice();
        } else if (selectedOption == 1) {
          disableBluetooth();
        } else if (selectedOption == 2) {
          startScan();
        } else if (selectedOption == 3) {
          clearBluetoothCache();
        } else {
          onGoBack();
        }
      } else {
        // Menu: Disable, Scan, Clear Cache, Back
        if (selectedOption == 0) {
          disableBluetooth();
        } else if (selectedOption == 1) {
          startScan();
        } else if (selectedOption == 2) {
          clearBluetoothCache();
        } else {
          onGoBack();
        }
      }
    }
  } else if (state == BluetoothSettingsState::SCANNING) {
    // Wait for initial scan to complete
    if (!BT_PAGE_TURNER.isScanning()) {
      devices = BT_PAGE_TURNER.getScannedDevices();

      // Start continuous scan to keep the list updated
      BT_PAGE_TURNER.startContinuousScan();
      state = BluetoothSettingsState::DEVICE_LIST;
      selectedDeviceIndex = 0;

      updateRequired = true;
    }
  } else if (state == BluetoothSettingsState::DEVICE_LIST) {
    // Update the list continuously
    if (BT_PAGE_TURNER.isContinuousScanning()) {
      static unsigned long lastScanTrigger = 0;
      if (millis() - lastScanTrigger > 2000) { // Restart scan every 2 seconds
        BT_PAGE_TURNER.startScan(2);
        lastScanTrigger = millis();
      }

      // Update the displayed list
      static unsigned long lastListUpdate = 0;
      if (millis() - lastListUpdate > 1000) { // UI update every 1s
        devices = BT_PAGE_TURNER.getScannedDevices();
        lastListUpdate = millis();
        updateRequired = true;
      }
    }

    // Navigation always possible, even during scan
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      BT_PAGE_TURNER.stopContinuousScan(); // Stop continuous scan
      state = BluetoothSettingsState::MENU;
      selectedOption = 0;
      updateRequired = true;
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Up) || mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!devices.empty() && selectedDeviceIndex > 0) {
        selectedDeviceIndex--;
        updateRequired = true;
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) || mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (!devices.empty() && selectedDeviceIndex < static_cast<int>(devices.size()) - 1) {
        selectedDeviceIndex++;
        updateRequired = true;
      }
    }

    // Connection possible even during scan
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !devices.empty()) {
      BT_PAGE_TURNER.stopContinuousScan(); // Stop scan before connecting
      connectToSelectedDevice();
    }
  } else if (state == BluetoothSettingsState::CONNECTING) {
    // Update display periodically to show status changes
    static unsigned long lastStatusUpdate = 0;
    if (millis() - lastStatusUpdate > 500) {  // Update every 500ms
      lastStatusUpdate = millis();
      updateRequired = true;
    }

    // Check if connection completed
    if (BT_PAGE_TURNER.isConnected()) {
      state = BluetoothSettingsState::MENU;
      selectedOption = 0;
      updateRequired = true;
    }

    // Allow user to cancel
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      BT_PAGE_TURNER.disconnect();
      state = BluetoothSettingsState::MENU;
      selectedOption = 0;
      updateRequired = true;
    }
  }
}

void BluetoothSettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void BluetoothSettingsActivity::render() const {
  renderer.clearScreen();

  switch (state) {
    case BluetoothSettingsState::MENU:
      renderMenu();
      break;
    case BluetoothSettingsState::SCANNING:
      renderScanning();
      break;
    case BluetoothSettingsState::DEVICE_LIST:
      renderDeviceList();
      break;
    case BluetoothSettingsState::CONNECTING:
      renderConnecting();
      break;
  }

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderMenu() const {
  const auto pageWidth = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, 10, "Bluetooth", true, BOLD);

  // Draw status
  const char* statusText = bluetoothEnabled ? "Status: Enabled" : "Status: Disabled";
  renderer.drawCenteredText(UI_10_FONT_ID, 60, statusText, true, REGULAR);

  // Draw connected device if applicable
  int startY = 120;
  if (bluetoothEnabled && BT_PAGE_TURNER.isConnected()) {
    std::string deviceName = BT_PAGE_TURNER.getConnectedDeviceName();
    if (deviceName.length() > 20) {
      deviceName.resize(17);
      deviceName += "...";
    }
    std::string connectedText = "Connected: " + deviceName;
    renderer.drawCenteredText(SMALL_FONT_ID, 90, connectedText.c_str(), true, BOLD);
    startY = 115;
  }

  constexpr int itemHeight = 40;
  int menuY = startY;
  int menuIndex = 0;

  if (!bluetoothEnabled) {
    // Menu: Enable, Back
    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Enable Bluetooth", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Enable Bluetooth", true);
    }
    menuY += itemHeight;
    menuIndex++;

    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Back", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Back", true);
    }
  } else if (BT_PAGE_TURNER.isConnected()) {
    // Menu: Disconnect, Disable, Scan, Clear Cache, Back
    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Disconnect Device", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Disconnect Device", true);
    }
    menuY += itemHeight;
    menuIndex++;

    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Disable Bluetooth", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Disable Bluetooth", true);
    }
    menuY += itemHeight;
    menuIndex++;

    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Scan for Devices", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Scan for Devices", true);
    }
    menuY += itemHeight;
    menuIndex++;

    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Clear Cache", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Clear Cache", true);
    }
    menuY += itemHeight;
    menuIndex++;

    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Back", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Back", true);
    }
  } else {
    // Menu: Disable, Scan, Clear Cache, Back
    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Disable Bluetooth", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Disable Bluetooth", true);
    }
    menuY += itemHeight;
    menuIndex++;

    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Scan for Devices", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Scan for Devices", true);
    }
    menuY += itemHeight;
    menuIndex++;

    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Clear Cache", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Clear Cache", true);
    }
    menuY += itemHeight;
    menuIndex++;

    if (selectedOption == menuIndex) {
      renderer.fillRect(20, menuY - 2, pageWidth - 40, itemHeight - 6);
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Back", false);
    } else {
      renderer.drawText(UI_10_FONT_ID, 30, menuY, "Back", true);
    }
  }

  renderer.drawButtonHints(UI_10_FONT_ID, "« Back", "Select", "", "");
}

void BluetoothSettingsActivity::renderScanning() const {
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 10, "Bluetooth Scan", true, BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, "Scanning...", true, BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2, "Turn ON your remote", true, REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 25, "control NOW while", true, REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 50, "scanning is active", true, REGULAR);
}

void BluetoothSettingsActivity::renderDeviceList() const {
  const auto pageWidth = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, 10, "Select Device", true, BOLD);

  // Continuous scan indicator
  if (BT_PAGE_TURNER.isContinuousScanning()) {
    renderer.drawCenteredText(SMALL_FONT_ID, 35, "Scanning...", true, REGULAR);
  }

  if (devices.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 100, "No devices found", true, REGULAR);
    renderer.drawCenteredText(SMALL_FONT_ID, 130, "Searching continuously...", true, REGULAR);
    renderer.drawButtonHints(UI_10_FONT_ID, "« Back", "", "", "");
    return;
  }

  constexpr int startY = 60;
  constexpr int lineHeight = 30;
  const int maxVisible = (renderer.getScreenHeight() - startY - 40) / lineHeight;

  int scrollOffset = 0;
  if (selectedDeviceIndex >= maxVisible) {
    scrollOffset = selectedDeviceIndex - maxVisible + 1;
  }

  for (int i = scrollOffset; i < static_cast<int>(devices.size()) && i < scrollOffset + maxVisible; i++) {
    const int y = startY + (i - scrollOffset) * lineHeight;
    const auto& device = devices[i];

    if (i == selectedDeviceIndex) {
      renderer.fillRect(0, y - 2, pageWidth, lineHeight);
    }

    std::string displayName = device.name;
    if (displayName.length() > 20) {
      displayName.resize(17);
      displayName += "...";
    }

    renderer.drawText(UI_10_FONT_ID, 10, y, displayName.c_str(), i != selectedDeviceIndex);

    // Display RSSI to see signal strength
    std::string rssiText = std::to_string(device.rssi) + " dBm";
    renderer.drawText(SMALL_FONT_ID, pageWidth - 80, y + 2, rssiText.c_str(), i != selectedDeviceIndex);
  }

  renderer.drawButtonHints(UI_10_FONT_ID, "« Back", "Connect", "", "");
}

void BluetoothSettingsActivity::renderConnecting() const {
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 10, "Bluetooth Pairing", true, BOLD);

  std::string status = BT_PAGE_TURNER.getConnectionStatus();

  // Larger, more visible main message
  if (status.find("Press button") != std::string::npos) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 80, "PAIRING IN PROGRESS", true, BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, "Press ANY button", true, BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, "on your remote", true, BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, "RIGHT NOW!", true, BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 60, "(within 10 seconds)", true, REGULAR);
  } else if (status.find("Connecting") != std::string::npos) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, "Connecting...", true, BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 10, "Please wait", true, REGULAR);
  } else if (status.find("Discovering") != std::string::npos) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, "Configuring device...", true, BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 10, "Please wait", true, REGULAR);
  } else if (status.find("Timeout") != std::string::npos || status.find("ERROR") != std::string::npos) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, "Connection Failed", true, BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 - 20, "Make sure device is in", true, REGULAR);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 5, "pairing mode and press", true, REGULAR);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 30, "a button quickly", true, REGULAR);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, status.c_str(), true, BOLD);
  }

  renderer.drawButtonHints(UI_10_FONT_ID, "« Cancel", "", "", "");
}

void BluetoothSettingsActivity::enableBluetooth() {
  Serial.printf("[%lu] [BTS] Enabling Bluetooth...\n", millis());

  if (BT_PAGE_TURNER.initialize()) {
    bluetoothEnabled = true;
    selectedOption = 0;
    updateRequired = true;
    Serial.printf("[%lu] [BTS] Bluetooth enabled\n", millis());
  } else {
    Serial.printf("[%lu] [BTS] Failed to enable Bluetooth\n", millis());
  }
}

void BluetoothSettingsActivity::disableBluetooth() {
  Serial.printf("[%lu] [BTS] Disabling Bluetooth...\n", millis());

  BT_PAGE_TURNER.shutdown();
  bluetoothEnabled = false;
  state = BluetoothSettingsState::MENU;
  selectedOption = 0;
  updateRequired = true;

  Serial.printf("[%lu] [BTS] Bluetooth disabled\n", millis());
}

void BluetoothSettingsActivity::disconnectDevice() {
  Serial.printf("[%lu] [BTS] Disconnecting device...\n", millis());

  BT_PAGE_TURNER.disconnect();
  selectedOption = 0;
  updateRequired = true;

  Serial.printf("[%lu] [BTS] Device disconnected\n", millis());
}

void BluetoothSettingsActivity::startScan() {
  Serial.printf("[%lu] [BTS] Starting scan...\n", millis());

  state = BluetoothSettingsState::SCANNING;
  updateRequired = true;

  vTaskDelay(100 / portTICK_PERIOD_MS);

  BT_PAGE_TURNER.startScan(10);
}

void BluetoothSettingsActivity::clearBluetoothCache() {
  Serial.printf("[%lu] [BTS] Clearing Bluetooth cache...\n", millis());

  BT_PAGE_TURNER.clearCache();
  selectedOption = 0;
  updateRequired = true;

  Serial.printf("[%lu] [BTS] Cache cleared\n", millis());
}

void BluetoothSettingsActivity::connectToSelectedDevice() {
  if (selectedDeviceIndex < 0 || selectedDeviceIndex >= static_cast<int>(devices.size())) {
    return;
  }

  const auto& device = devices[selectedDeviceIndex];
  connectingToAddress = device.address;

  Serial.printf("[%lu] [BTS] Connecting to %s (%s)...\n",
                millis(), device.name.c_str(), device.address.c_str());

  state = BluetoothSettingsState::CONNECTING;
  updateRequired = true;

  vTaskDelay(100 / portTICK_PERIOD_MS);

  if (BT_PAGE_TURNER.connectToDevice(device.address)) {
    state = BluetoothSettingsState::MENU;
    selectedOption = 0;
    updateRequired = true;
    Serial.printf("[%lu] [BTS] Connected successfully\n", millis());
  } else {
    Serial.printf("[%lu] [BTS] Connection failed\n", millis());
    state = BluetoothSettingsState::MENU;
    selectedOption = 0;
    updateRequired = true;
  }
}