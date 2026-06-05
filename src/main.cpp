#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

int scanTime = 5; // In seconds
BLEScan* pBLEScan;

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      Serial.printf("Advertised Device: %s \n", advertisedDevice.toString().c_str());
      if (advertisedDevice.haveName()) {
        Serial.printf("  Name: %s\n", advertisedDevice.getName().c_str());
      }
      if (advertisedDevice.haveServiceUUID()) {
        Serial.printf("  Service UUID: %s\n", advertisedDevice.getServiceUUID().toString().c_str());
      }
    }
};

void setup() {
  Serial.begin(115200);
  while(!Serial) { delay(10); }
  Serial.println("ESP32 BLE Scanner starting...");

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan(); //create new scan
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true); //active scan uses more power, but get results faster
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);  // less or equal setInterval value
}

void loop() {
  Serial.println("Scanning for BLE devices...");
  BLEScanResults* foundDevices = pBLEScan->start(scanTime, false);
  Serial.printf("Scan done! Found %d devices.\n", foundDevices->getCount());
  pBLEScan->clearResults();   // delete results fromBLEScan buffer to release memory
  delay(2000);
}
