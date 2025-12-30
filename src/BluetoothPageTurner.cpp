#include "BluetoothPageTurner.h"
#include <Arduino.h>
#include <map>

// Instance globale
BluetoothPageTurner BT_PAGE_TURNER;

// UUIDs standard pour HID
static BLEUUID hidServiceUUID((uint16_t)0x1812);
static BLEUUID hidReportCharUUID((uint16_t)0x2A4D);
static BLEUUID reportMapCharUUID((uint16_t)0x2A4B);
static BLEUUID hidInfoCharUUID((uint16_t)0x2A4A);

// Global map to accumulate scan results from callback
static std::map<std::string, BluetoothPageTurner::ScannedDevice> g_scannedDevicesMap;

// Classe pour recevoir les callbacks de scan en temps réel
class AdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        std::string name = advertisedDevice.haveName() ? advertisedDevice.getName() : "Unknown";
        std::string address = advertisedDevice.getAddress().toString();
        int rssi = advertisedDevice.getRSSI();

        Serial.printf("[%lu] [BT] DETECTED: %s (%s) RSSI: %d\n",
                     millis(), name.c_str(), address.c_str(), rssi);

        // Afficher les services si disponibles
        if (advertisedDevice.haveServiceUUID()) {
            Serial.printf("[%lu] [BT]   Services: ", millis());
            for (int i = 0; i < advertisedDevice.getServiceUUIDCount(); i++) {
                Serial.printf("%s ", advertisedDevice.getServiceUUID(i).toString().c_str());
            }
            Serial.println();
        }

        // Store in global map
        BluetoothPageTurner::ScannedDevice device;
        device.name = name;
        device.address = address;
        device.rssi = rssi;
        device.isBLE = true;
        g_scannedDevicesMap[address] = device;
    }
};

bool BluetoothPageTurner::initialize() {
    if (initialized) {
        Serial.printf("[%lu] [BT] Already initialized\n", millis());
        return true;
    }

    Serial.printf("[%lu] [BT] Initializing BLE (ESP32-C3)...\n", millis());

    BLEDevice::init("CrossPoint Reader");

    pBLEScan = BLEDevice::getScan();
    if (!pBLEScan) {
        Serial.printf("[%lu] [BT] Failed to create BLE scan\n", millis());
        return false;
    }

    // Very aggressive scan configuration to catch fleeting devices
    pBLEScan->setActiveScan(true);     // Active scan to get scan response
    pBLEScan->setInterval(100);        // 100ms interval (recommended default)
    pBLEScan->setWindow(99);           // Scan almost all the time

    // IMPORTANT: Register callback to get real-time results
    pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(), true);

    bleClient = BLEDevice::createClient();
    if (!bleClient) {
        Serial.printf("[%lu] [BT] Failed to create BLE client\n", millis());
        return false;
    }

    initialized = true;
    Serial.printf("[%lu] [BT] BLE initialized successfully\n", millis());
    return true;
}

void BluetoothPageTurner::shutdown() {
    if (!initialized) {
        return;
    }

    Serial.printf("[%lu] [BT] Shutting down BLE...\n", millis());

    disconnect();

    if (bleClient) {
        delete bleClient;
        bleClient = nullptr;
    }

    BLEDevice::deinit(true);

    initialized = false;
    scanning = false;
    connected = false;
    scannedDevices.clear();

    Serial.printf("[%lu] [BT] BLE shutdown complete\n", millis());
}

bool BluetoothPageTurner::startScan(int durationSeconds) {
    if (!initialized) {
        Serial.printf("[%lu] [BT] Cannot scan: not initialized\n", millis());
        return false;
    }

    if (scanning) {
        Serial.printf("[%lu] [BT] Scan already in progress\n", millis());
        return false;
    }

    Serial.printf("[%lu] [BT] Starting AGGRESSIVE BLE scan for %d seconds...\n", millis(), durationSeconds);
    Serial.printf("[%lu] [BT] Turn ON your remote NOW!\n", millis());

    // Clear list if not in continuous mode
    if (!continuousScanning) {
        scannedDevices.clear();
        g_scannedDevicesMap.clear(); // Clear global map too
    }

    scanning = true;

    // Start scan - results will be accumulated in g_scannedDevicesMap via callback
    pBLEScan->start(durationSeconds, false);

    Serial.printf("[%lu] [BT] Scan complete, processing %d results from callback\n", millis(), g_scannedDevicesMap.size());

    // Use a map to avoid duplicates by address
    std::map<std::string, ScannedDevice> deviceMap;

    // Keep old devices in the map
    for (const auto& device : scannedDevices) {
        deviceMap[device.address] = device;
    }

    // Add devices from global map (populated by callback)
    for (const auto& pair : g_scannedDevicesMap) {
        deviceMap[pair.first] = pair.second;
    }

    // Rebuild scannedDevices from the map
    scannedDevices.clear();
    for (const auto& pair : deviceMap) {
        scannedDevices.push_back(pair.second);
    }

    // Sort by RSSI descending (strongest signal first)
    std::sort(scannedDevices.begin(), scannedDevices.end(),
              [](const ScannedDevice& a, const ScannedDevice& b) {
                  return a.rssi > b.rssi;
              });

    Serial.printf("[%lu] [BT] Found %d unique BLE devices\n", millis(), scannedDevices.size());

    pBLEScan->clearResults();
    scanning = false;

    return true;
}

bool BluetoothPageTurner::connectToDevice(const std::string& address) {
    if (!initialized || scanning) {
        return false;
    }

    connectionStatus = "Connecting...";
    Serial.printf("[%lu] [BT] Connecting to %s...\n", millis(), address.c_str());

    currentAddress = address;
    connected = false;
    maxRetries = 3;
    currentRetry = 0;

    return attemptConnection();
}

bool BluetoothPageTurner::attemptConnection() {
    if (currentRetry >= maxRetries) {
        connectionStatus = "Connection failed";
        Serial.printf("[%lu] [BT] Connection failed after %d retries\n", millis(), maxRetries);
        return false;
    }

    currentRetry++;
    connectionStatus = "Attempting connection...";
    Serial.printf("[%lu] [BT] Connection attempt %d/%d...\n", millis(), currentRetry, maxRetries);

    BLEAddress bleAddress(currentAddress.c_str());

    if (!bleClient->connect(bleAddress)) {
        connectionStatus = "Connection failed";
        Serial.printf("[%lu] [BT] Connection attempt %d failed\n", millis(), currentRetry);

        if (currentRetry < maxRetries) {
            connectionStatus = "Retrying...";
            delay(1000);
            return attemptConnection();
        }
        return false;
    }

    connectionStatus = "Connected! Configuring...";
    Serial.printf("[%lu] [BT] Connected to device\n", millis());

    delay(500);

    if (!bleClient->isConnected()) {
        connectionStatus = "Device disconnected";
        Serial.printf("[%lu] [BT] ERROR: Device disconnected immediately\n", millis());
        Serial.printf("[%lu] [BT] Make sure device is in pairing mode\n", millis());
        return false;
    }

    connectionStatus = "Discovering services...";
    Serial.printf("[%lu] [BT] Discovering services...\n", millis());

    if (!discoverServicesAndCharacteristics()) {
        connectionStatus = "Service discovery failed";
        Serial.printf("[%lu] [BT] Failed to discover services\n", millis());
        bleClient->disconnect();
        return false;
    }

    connectionStatus = "Press button on remote NOW";
    Serial.printf("[%lu] [BT] Waiting for button press (10 seconds)...\n", millis());

    unsigned long startWait = millis();
    bool buttonPressed = false;

    while (millis() - startWait < 10000) {
        if (!bleClient->isConnected()) {
            connectionStatus = "ERROR: Disconnected!";
            Serial.printf("[%lu] [BT] ERROR: Device disconnected while waiting\n", millis());
            Serial.printf("[%lu] [BT] Press ANY button on remote during pairing!\n", millis());
            return false;
        }

        if (lastKeyPressed != KeyCode::NONE) {
            buttonPressed = true;
            Serial.printf("[%lu] [BT] Button press detected!\n", millis());
            break;
        }

        delay(100);
    }

    if (!buttonPressed) {
        connectionStatus = "Timeout: No button press";
        Serial.printf("[%lu] [BT] ERROR: No button press within 10 seconds\n", millis());
        Serial.printf("[%lu] [BT] Press ANY button quickly after connecting!\n", millis());
        bleClient->disconnect();
        return false;
    }

    connected = true;
    connectedDeviceName = currentAddress;
    connectionStatus = "Connected successfully";
    Serial.printf("[%lu] [BT] Successfully paired!\n", millis());

    return true;
}

bool BluetoothPageTurner::discoverServicesAndCharacteristics() {
    if (!bleClient || !bleClient->isConnected()) {
        return false;
    }

    Serial.printf("[%lu] [BT] Discovering HID service...\n", millis());

    BLERemoteService* pRemoteService = bleClient->getService(hidServiceUUID);
    if (!pRemoteService) {
        Serial.printf("[%lu] [BT] HID service (0x1812) not found\n", millis());

        // Lister tous les services disponibles pour debug
        Serial.printf("[%lu] [BT] Available services:\n", millis());
        std::map<std::string, BLERemoteService*>* services = bleClient->getServices();
        for (auto& service : *services) {
            Serial.printf("[%lu] [BT]   - %s\n", millis(), service.first.c_str());
        }

        return false;
    }

    Serial.printf("[%lu] [BT] HID service found\n", millis());

    hidReportCharacteristic = pRemoteService->getCharacteristic(hidReportCharUUID);
    if (!hidReportCharacteristic) {
        Serial.printf("[%lu] [BT] HID report characteristic (0x2A4D) not found\n", millis());

        // Lister toutes les caractéristiques pour debug
        Serial.printf("[%lu] [BT] Available characteristics:\n", millis());
        std::map<std::string, BLERemoteCharacteristic*>* characteristics = pRemoteService->getCharacteristics();
        for (auto& characteristic : *characteristics) {
            Serial.printf("[%lu] [BT]   - %s\n", millis(), characteristic.first.c_str());
        }

        return false;
    }

    Serial.printf("[%lu] [BT] HID report characteristic found\n", millis());

    if (hidReportCharacteristic->canNotify()) {
        hidReportCharacteristic->registerForNotify(notifyCallback);
        Serial.printf("[%lu] [BT] Registered for HID notifications\n", millis());
    } else {
        Serial.printf("[%lu] [BT] WARNING: Characteristic cannot notify\n", millis());
    }

    return true;
}

void BluetoothPageTurner::notifyCallback(BLERemoteCharacteristic* pCharacteristic,
                                         uint8_t* pData, size_t length, bool isNotify) {
    if (length < 1) {
        return;
    }

    Serial.printf("[%lu] [BT] HID Report (%d bytes): ", millis(), length);
    for (size_t i = 0; i < length; i++) {
        Serial.printf("%02X ", pData[i]);
    }
    Serial.println();

    // Format HID standard : [modifier, reserved, keycode1, keycode2, ...]
    // PAGE_UP = 0x4B, PAGE_DOWN = 0x4E
    if (length >= 3) {
        uint8_t keyCode = pData[2];

        if (keyCode == 0x4B) {
            BT_PAGE_TURNER.lastKeyPressed = KeyCode::PAGE_UP;
            Serial.printf("[%lu] [BT] PAGE_UP detected\n", millis());
        } else if (keyCode == 0x4E) {
            BT_PAGE_TURNER.lastKeyPressed = KeyCode::PAGE_DOWN;
            Serial.printf("[%lu] [BT] PAGE_DOWN detected\n", millis());
        } else if (keyCode != 0x00) {
            // N'importe quelle touche pour l'appairage
            BT_PAGE_TURNER.lastKeyPressed = KeyCode::PAGE_UP;
            Serial.printf("[%lu] [BT] Button press (0x%02X)\n", millis(), keyCode);
        }
    }
}

void BluetoothPageTurner::disconnect() {
    if (!bleClient) {
        return;
    }

    Serial.printf("[%lu] [BT] Disconnecting...\n", millis());

    if (bleClient->isConnected()) {
        bleClient->disconnect();
    }

    connected = false;
    connectedDeviceName = "";
    hidReportCharacteristic = nullptr;
    connectionStatus = "Disconnected";

    Serial.printf("[%lu] [BT] Disconnected\n", millis());
}

BluetoothPageTurner::KeyCode BluetoothPageTurner::getLastKeyPressed() {
    KeyCode key = lastKeyPressed;
    lastKeyPressed = KeyCode::NONE;
    return key;
}

void BluetoothPageTurner::clearCache() {
    Serial.printf("[%lu] [BT] Clearing cache...\n", millis());

    if (connected) {
        disconnect();
    }

    // Effacer la liste des appareils scannés
    scannedDevices.clear();

    Serial.printf("[%lu] [BT] Cache cleared\n", millis());
}

bool BluetoothPageTurner::startContinuousScan() {
  if (!initialized) {
    Serial.printf("[%lu] [BT] Cannot start continuous scan: not initialized\n", millis());
    return false;
  }

  Serial.printf("[%lu] [BT] Starting continuous scan mode...\n", millis());

  continuousScanning = true;
  lastScanUpdate = 0;

  return true;
}

void BluetoothPageTurner::stopContinuousScan() {
  if (continuousScanning) {
    Serial.printf("[%lu] [BT] Stopping continuous scan mode\n", millis());
    continuousScanning = false;

    if (scanning) {
      pBLEScan->stop();
      scanning = false;
    }
  }
}