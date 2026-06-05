#ifndef OBD_MANAGER_H
#define OBD_MANAGER_H

#include <Arduino.h>
#include "buffer.h"

#include <BLEDevice.h>

// Initialize the OBD2 BLE client connection
bool connectOBD(BLEAdvertisedDevice* device);

// Disconnect from Wican OBD BLE server
void disconnectOBD();

// Periodic keep-alive task (Tester Present)
void runOBDKeepAlive();

// Query all Group A cyclic parameters (every 60s)
bool queryGroupA(TelemetryData& data);

// Query all Group B slow poll parameters (every 10min)
bool queryGroupB(TelemetryData& data);

// Run the full simulation fallback (in case Wican or OBD queries fail)
void generateSimulatedTelemetry(TelemetryData& data, bool slowGroup);

// Get the latest successfully read 12V board voltage (AT RV)
float getLatestRealVoltage();

#endif // OBD_MANAGER_H
