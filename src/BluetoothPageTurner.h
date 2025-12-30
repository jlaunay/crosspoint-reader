#pragma once
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <string>
#include <vector>

class BluetoothPageTurner {
public:
  struct ScannedDevice {
    std::string name;
    std::string address;
    int rssi;
    bool isBLE;  // true = BLE, false = Classic
  };

  enum class KeyCode {
    NONE = 0,
    PAGE_UP = 1,
    PAGE_DOWN = 2,
  };

  // Scan results (public pour affichage)
  std::vector<ScannedDevice> scannedDevices;

  bool startContinuousScan();
  void stopContinuousScan();
  bool isContinuousScanning() const { return continuousScanning; }

private:
  bool initialized = false;

  // BLE uniquement (ESP32-C3 ne supporte pas Classic pour HID)
  BLEScan* pBLEScan = nullptr;
  BLEClient* bleClient = nullptr;
  BLERemoteCharacteristic* hidReportCharacteristic = nullptr;
  bool continuousScanning = false;
  unsigned long lastScanUpdate = 0;

  // État de connexion
  bool connected = false;
  std::string connectedDeviceName;
  std::string connectionStatus;

  // Gestion des reconnexions
  std::string currentAddress;
  int maxRetries = 3;
  int currentRetry = 0;

  // Scan
  bool scanning = false;

  // HID
  KeyCode lastKeyPressed = KeyCode::NONE;

  // Méthodes privées
  bool attemptConnection();
  bool discoverServicesAndCharacteristics();
  static void notifyCallback(BLERemoteCharacteristic* pCharacteristic, uint8_t* pData, size_t length, bool isNotify);

  // Callback pour détecter les appareils en temps réel pendant le scan
  static void scanCallback(BLEAdvertisedDevice advertisedDevice);

public:
  BluetoothPageTurner() = default;

  // Initialisation et shutdown
  bool initialize();
  void shutdown();
  bool isInitialized() const { return initialized; }

  // Scan (BLE uniquement sur ESP32-C3)
  bool startScan(int durationSeconds);
  bool isScanning() const { return scanning; }
  std::vector<ScannedDevice> getScannedDevices() const { return scannedDevices; }

  // Connexion
  bool connectToDevice(const std::string& address);
  void disconnect();
  bool isConnected() const { return connected; }
  std::string getConnectedDeviceName() const { return connectedDeviceName; }
  std::string getConnectionStatus() const { return connectionStatus; }

  // HID
  KeyCode getLastKeyPressed();

  // Cache
  void clearCache();
};

// Instance globale
extern BluetoothPageTurner BT_PAGE_TURNER;