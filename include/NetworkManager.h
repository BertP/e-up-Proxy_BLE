#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "buffer.h" // Needed for TelemetryData struct

enum ProxyState {
    STATE_SCANNING,
    STATE_CONNECTED_TO_WICAN,
    STATE_CONNECTED_TO_HOME
};

enum WiFiConnectPhase {
    WIFI_IDLE,
    WIFI_CONNECTING,
    WIFI_CONNECTED
};

// Shared global instances
extern WiFiClient wifiClient;
extern PubSubClient mqttClient;
extern ProxyState currentState;
extern const char* stateNames[];

extern bool otaInProgress;
extern bool webServerRunning;
extern bool timeSyncDone;

void initNetwork();
void runWiFiStateMachine();
void transitionTo(ProxyState newState, const String& reason = "");
void handleScanning();
void handleHome();
void flushQueueToMQTT();
void publishHAAutoDiscovery();
void fetchOBDMetrics(bool forceSlow = false);

#endif // NETWORK_MANAGER_H
