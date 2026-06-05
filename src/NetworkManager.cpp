#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include "NetworkManager.h"
#include "config.h"
#include "logger.h"
#include "buffer.h"
#include "OBDManager.h"

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
ProxyState currentState = STATE_SCANNING;
const char* stateNames[] = { "SCANNING", "OPERATIONAL_BUFFERING", "OPERATIONAL_ONLINE" };


bool webServerRunning = false;
bool timeSyncDone = false;
static bool isWebServerStarted = false;

bool mqttFlushDone = false;
extern bool obdActive;

// Shared variables from main
#include <WebServer.h>
extern WebServer server;
extern Preferences prefs;
extern TelemetryData latestCachedData;
extern uint32_t obdSessionSuccessCount;
extern uint32_t obdSessionFailCount;
extern unsigned long obdSessionStartTime;
extern unsigned long lastSlowTelemetryFetch;
extern unsigned long lastTelemetryFetch;
extern void resetBootCrashCounter();
extern void feedWDT();
extern bool g_ntpSynchronized;

static BLEScan* pBLEScan = nullptr;
static bool isBleScanning = false;
static BLEAdvertisedDevice* foundWicanDevice = nullptr;

static unsigned long wifiOnlineStartTime = 0;
static bool isWifiScanning = false;
static unsigned long wifiScanStartTime = 0;

static WiFiConnectPhase wifiConnectPhase = WIFI_IDLE;
static unsigned long wifiConnectStartTime = 0;
static String pendingSSID = "";
static String pendingPass = "";

static void onBleScanComplete(BLEScanResults foundDevices) {
    for (int i = 0; i < foundDevices.getCount(); i++) {
        BLEAdvertisedDevice dev = foundDevices.getDevice(i);
        String name = dev.haveName() ? String(dev.getName().c_str()) : "";
        if (name.startsWith(WICAN_BLE_PREFIX)) {
            logEvent("SCAN", "Found WiCAN BLE: " + name + " (RSSI: " + String(dev.getRSSI()) + ")");
            foundWicanDevice = new BLEAdvertisedDevice(dev);
            break;
        }
    }
    pBLEScan->clearResults();
    isBleScanning = false;
}

void initNetwork() {
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
}

void evaluateOverallState() {
    ProxyState newState = STATE_SCANNING;
    if (WiFi.status() == WL_CONNECTED) {
        newState = STATE_OPERATIONAL_ONLINE;
    } else if (obdActive) {
        newState = STATE_OPERATIONAL_BUFFERING;
    }

    if (newState != currentState) {
        logEvent("STATE", "Transition to " + String(stateNames[newState]));
        currentState = newState;
    }
}

void transitionTo(ProxyState newState, const String& reason) {
    // Left for compatibility with external references, but evaluateOverallState is preferred internally
    evaluateOverallState();
}

static void connectToHomeWiFi(const String& ssid, const String& pass) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    wifiConnectPhase = WIFI_CONNECTING;
    wifiConnectStartTime = millis();
    pendingSSID = ssid;
    pendingPass = pass;
    logEvent("SWITCH", "Connecting to Home WiFi: \"" + ssid + "\"");
    WiFi.begin(ssid.c_str(), pass.c_str());
}

static void checkWiFiConnect() {
    if (wifiConnectPhase != WIFI_CONNECTING) return;
    
    if (WiFi.status() == WL_CONNECTED) {
        logEvent("SWITCH", "Connected to WiFi. IP: " + WiFi.localIP().toString());
        wifiConnectPhase = WIFI_CONNECTED;
        wifiOnlineStartTime = millis();
        mqttFlushDone = false;
        
        configTzTime(TZ_INFO, NTP_SERVER);
        
        webServerRunning = true;
        if (!isWebServerStarted) {
            server.begin();
            isWebServerStarted = true;
            logEvent("WEBSERVER", "Debug server started at http://" + WiFi.localIP().toString() + "/debug");
        }
        
        evaluateOverallState();
        return;
    }
    
    if (millis() - wifiConnectStartTime >= WIFI_CONNECT_TIMEOUT_HOME_MS) {
        logEvent("ERROR", "Failed to connect to " + pendingSSID + " (timeout)");
        WiFi.disconnect(true);
        wifiConnectPhase = WIFI_IDLE;
    }
}

static void handleWiFiLogic() {
    if (wifiConnectPhase == WIFI_CONNECTING) {
        checkWiFiConnect();
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (webServerRunning) server.handleClient();


        if (!timeSyncDone) {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 100)) {
                if (timeinfo.tm_year > 120) {
                    char timeBuf[16], tzBuf[10], offBuf[10];
                    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);
                    strftime(tzBuf, sizeof(tzBuf), "%Z", &timeinfo);
                    strftime(offBuf, sizeof(offBuf), "%z", &timeinfo);
                    String offStr = String(offBuf);
                    if (offStr.length() == 5) offStr = offStr.substring(0, 3) + ":" + offStr.substring(3);
                    logEvent("BOOT", "NTP synchronised. Local time: " + String(timeBuf) + " (Europe/Berlin, " + String(tzBuf) + " " + offStr + ")");
                    timeSyncDone = true;
                    g_ntpSynchronized = true;
                    resetBootCrashCounter();
                }
            }
        }

        if (!mqttFlushDone && timeSyncDone) {
            flushQueueToMQTT();
        }

        if ((millis() - wifiOnlineStartTime > 600000)) { // 10 minutes timeout
            logEvent("SWITCH", "10-minute Home WiFi timeout reached. Disabling Wi-Fi to save power.");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            wifiConnectPhase = WIFI_IDLE;
            webServerRunning = false;
            evaluateOverallState();
        }
        return;
    }

    // WiFi is disconnected. Should we scan?
    if (getQueueSize() > 0 && !isWifiScanning && wifiConnectPhase == WIFI_IDLE) {
        WiFi.mode(WIFI_STA);
        logEvent("SCAN", "Buffered telemetry found. Scanning for Home WiFi...");
        WiFi.scanNetworks(true, false);
        isWifiScanning = true;
        wifiScanStartTime = millis();
    } else if (isWifiScanning) {
        int scanResult = WiFi.scanComplete();
        if (scanResult >= 0) {
            bool home1Found = false, home2Found = false;
            for (int i = 0; i < scanResult; i++) {
                String ssid = WiFi.SSID(i);
                if (ssid == HOME_SSID_1) home1Found = true;
                if (ssid == HOME_SSID_2) home2Found = true;
            }
            WiFi.scanDelete();
            isWifiScanning = false;
            
            if (home1Found) {
                connectToHomeWiFi(HOME_SSID_1, HOME_PASS_1);
            } else if (home2Found) {
                connectToHomeWiFi(HOME_SSID_2, HOME_PASS_2);
            } else {
                logEvent("SCAN", "No known Home WiFi found. Will retry later.");
            }
        } else if (scanResult == WIFI_SCAN_FAILED || millis() - wifiScanStartTime > 15000) {
            WiFi.scanDelete();
            isWifiScanning = false;
        }
    }
}

static void handleBLELogic() {
    if (obdActive) {
        runOBDKeepAlive();
        
        unsigned long currentMillis = millis();
        if (currentMillis - lastSlowTelemetryFetch >= POLL_INTERVAL_SLOW_MS) {
            lastSlowTelemetryFetch = currentMillis;
            fetchOBDMetrics(true);
            lastTelemetryFetch = currentMillis;
        }
        else if (currentMillis - lastTelemetryFetch >= POLL_INTERVAL_FAST_MS) {
            lastTelemetryFetch = currentMillis;
            fetchOBDMetrics(false);
        }
        return;
    }

    if (foundWicanDevice != nullptr) {
        // Safety: ensure scanner is fully stopped
        pBLEScan->stop();
        pBLEScan->clearResults();
        isBleScanning = false;

        delay(100); // Give the RF hardware a moment

        obdActive = connectOBD(foundWicanDevice);
        if (obdActive) {
            obdSessionStartTime = millis();
            obdSessionSuccessCount = 0;
            obdSessionFailCount = 0;
            fetchOBDMetrics(true);
            lastTelemetryFetch = millis();
            lastSlowTelemetryFetch = millis();
            evaluateOverallState();
        }
        delete foundWicanDevice;
        foundWicanDevice = nullptr;
    } else if (!isBleScanning) {
        // Only log sporadically to avoid spam
        pBLEScan->start(BLE_SCAN_TIME_S, onBleScanComplete, false);
        isBleScanning = true;
    }
}

void runNetworkStateMachine() {
    handleBLELogic();
    handleWiFiLogic();
}

void publishHAAutoDiscovery() {
    if (!mqttClient.connected()) return;
    logMqttEvent("CONN", "Publishing Home Assistant Auto-Discovery sensors...");

    struct SensorDef {
        const char* id;
        const char* name;
        const char* unit;
        const char* devClass;
        const char* valTpl;
        const char* icon;
    };

    SensorDef sensors[] = {
        {"soc", "Battery SoC", "%", "battery", "{{ value_json.soc }}", nullptr},
        {"volt", "12V Voltage", "V", "voltage", "{{ value_json.volt }}", nullptr},
        {"temp", "Battery Temperature", "C", "temperature", "{{ value_json.temp }}", nullptr},
        {"range", "Estimated Range", "km", nullptr, "{{ value_json.range }}", "mdi:car-range"},
        {"power", "Current Power", "W", "power", "{{ value_json.power }}", nullptr},
        {"odo", "Odometer", "km", nullptr, "{{ value_json.odo }}", "mdi:counter"},
        {"service_days", "Days to Service", "d", nullptr, "{{ value_json.service_days }}", "mdi:calendar-check"},
        {"service_km", "Distance to Service", "km", nullptr, "{{ value_json.service_km }}", "mdi:map-marker-distance"},
        {"bat_cap", "Battery Capacity", "Ah", nullptr, "{{ value_json.bat_cap }}", "mdi:battery-charging-100"},
        {"tp_alarm", "Tire Pressure Alarm", nullptr, nullptr, "{{ value_json.tp_alarm }}", "mdi:car-tire-alert"}
    };

    for (const auto& s : sensors) {
        String topic = "homeassistant/sensor/eup_proxy_" + String(s.id) + "/config";
        JsonDocument doc;
        doc["name"] = "e-up! " + String(s.name);
        doc["stat_t"] = MQTT_TOPIC_DATA;
        if (s.unit) doc["unit_of_meas"] = s.unit;
        if (s.devClass) doc["dev_class"] = s.devClass;
        doc["state_class"] = "measurement";
        doc["val_tpl"] = s.valTpl;
        doc["uniq_id"] = "eup_proxy_" + String(s.id);
        if (s.icon) doc["ic"] = s.icon;

        JsonObject dev = doc["dev"].to<JsonObject>();
        dev["ids"] = "eup_proxy_esp32";
        dev["name"] = "e-up! Proxy";
        dev["mf"] = "VW / local";
        dev["mdl"] = HARDWARE_PLATFORM;
        dev["sw"] = FW_VERSION;

        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(topic.c_str(), payload.c_str(), true);

        feedWDT();
        delay(20);
    }

    {
        String topic = "homeassistant/sensor/eup_proxy_lastsync/config";
        JsonDocument doc;
        doc["name"] = "e-up! Last Sync";
        doc["stat_t"] = MQTT_TOPIC_LASTSYNC;
        doc["dev_class"] = "timestamp";
        doc["uniq_id"] = "eup_proxy_lastsync";
        doc["ic"] = "mdi:sync";

        JsonObject dev = doc["dev"].to<JsonObject>();
        dev["ids"] = "eup_proxy_esp32";
        dev["name"] = "e-up! Proxy";
        dev["mf"] = "VW / local";
        dev["mdl"] = HARDWARE_PLATFORM;
        dev["sw"] = FW_VERSION;

        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(topic.c_str(), payload.c_str(), true);

        feedWDT();
        delay(20);
    }

    logMqttEvent("CONN", "Home Assistant Auto-Discovery published.");
}

void flushQueueToMQTT() {
    if (!mqttClient.connected()) {
        logMqttEvent("MQTT", "Connecting to Broker " + String(MQTT_HOST) + "...");
        String clientID = "eupProxy_" + String(ESP.getEfuseMac(), HEX);
        bool connected = false;
        if (strlen(MQTT_USER) > 0) {
            connected = mqttClient.connect(clientID.c_str(), MQTT_USER, MQTT_PASS);
        } else {
            connected = mqttClient.connect(clientID.c_str());
        }

        if (!connected) {
            logMqttEvent("ERROR", "MQTT connection failed, state: " + String(mqttClient.state()));
            return;
        }
        logMqttEvent("MQTT", "Successfully connected to Broker.");
        publishHAAutoDiscovery();
    }

    size_t queueSize = getQueueSize();
    if (queueSize == 0) {
        logMqttEvent("MQTT", "No buffered data to flush.");
        mqttFlushDone = true;
        return;
    }

    logMqttEvent("MQTT", "Found " + String(queueSize) + " records to flush. Starting...");
    time_t bootEpoch = time(nullptr) - (millis() / 1000);

    String filepath;
    TelemetryData data;
    int flushCount = 0;

    while (getNextQueuedFile(filepath, data)) {
        time_t correctedTs = data.ts;
        if (data.ts < 1000000000U) correctedTs = bootEpoch + data.ts;

        JsonDocument doc;
        doc["soc"] = std::isnan(data.soc) ? JsonVariant() : data.soc;
        doc["volt"] = std::isnan(data.volt) ? JsonVariant() : data.volt;
        doc["temp"] = std::isnan(data.temp) ? JsonVariant() : data.temp;
        doc["range"] = std::isnan(data.range) ? JsonVariant() : data.range;
        doc["power"] = std::isnan(data.power) ? JsonVariant() : data.power;
        doc["odo"] = std::isnan(data.odo) ? JsonVariant() : data.odo;
        doc["service_days"] = std::isnan(data.service_days) ? JsonVariant() : data.service_days;
        doc["service_km"] = std::isnan(data.service_km) ? JsonVariant() : data.service_km;
        doc["bat_cap"] = std::isnan(data.bat_cap) ? JsonVariant() : data.bat_cap;
        doc["tp_alarm"] = std::isnan(data.tp_alarm) ? JsonVariant() : data.tp_alarm;
        doc["ts"] = correctedTs;
        doc["src"] = data.src;

        String payload;
        serializeJson(doc, payload);

        logMqttEvent("MQTT", "Publishing to topic " + String(MQTT_TOPIC_DATA) + ": " + payload);
        if (mqttClient.publish(MQTT_TOPIC_DATA, payload.c_str(), true)) {
            removeQueuedFile(filepath);
            flushCount++;
        } else {
            logMqttEvent("ERROR", "Failed to publish message to topic " + String(MQTT_TOPIC_DATA) + "!");
            break;
        }

        feedWDT();
        delay(5);
    }

    logMqttEvent("MQTT", "Flush finished. Dispatched " + String(flushCount) + " messages.");

    struct tm timeinfo;
    char timeBuf[64] = "2026-05-31T11:05:04+02:00";
    if (getLocalTime(&timeinfo, 100)) {
        char zBuf[10];
        strftime(zBuf, sizeof(zBuf), "%z", &timeinfo);
        String tzStr = String(zBuf);
        if (tzStr.length() == 5) tzStr = tzStr.substring(0, 3) + ":" + tzStr.substring(3);
        char tBuf[32];
        strftime(tBuf, sizeof(tBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
        snprintf(timeBuf, sizeof(timeBuf), "%s%s", tBuf, tzStr.c_str());
    }

    logMqttEvent("MQTT", "Updating eup/lastSync: " + String(timeBuf));
    mqttClient.publish(MQTT_TOPIC_LASTSYNC, timeBuf, true);
    mqttFlushDone = true;
}
