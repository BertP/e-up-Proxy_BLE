#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "NetworkManager.h"
#include "config.h"
#include "logger.h"
#include "buffer.h"
#include "OBDManager.h"

// Define shared instances
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
ProxyState currentState = STATE_SCANNING;
const char* stateNames[] = { "SCANNING", "CONNECTED_TO_WICAN", "CONNECTED_TO_HOME" };

// Network state variables
static WiFiConnectPhase wifiConnectPhase = WIFI_IDLE;
static unsigned long wifiConnectStartTime = 0;
static String pendingSSID = "";
static String pendingPass = "";
static ProxyState pendingTargetState = STATE_SCANNING;
static unsigned long wifiConnectTimeout = 0;

static bool isScanningActive = false;
static bool lastScanFailed = false;
static unsigned long scanStartTime = 0;

bool otaInProgress = false;
bool webServerRunning = false;
bool timeSyncDone = false;
static bool isWebServerStarted = false;
static bool otaInitialized = false;
bool mqttFlushDone = false;
static unsigned long lastHomeRescanCheck = 0;

// External server defined in main/diagnostics
#include <WebServer.h>
extern WebServer server;
extern Preferences prefs;
extern TelemetryData latestCachedData;
extern bool obdActive;
extern uint32_t obdSessionSuccessCount;
extern uint32_t obdSessionFailCount;
extern unsigned long obdSessionStartTime;
extern unsigned long lastSlowTelemetryFetch;
extern unsigned long lastTelemetryFetch;
extern void resetBootCrashCounter();
extern void feedWDT();

// WiFi connect helper
static void beginWiFiConnect(const String& ssid, const String& pass, ProxyState targetState, unsigned long timeout) {
    WiFi.disconnect(true);
    wifiConnectStartTime = millis();
    wifiConnectTimeout = timeout;
    pendingSSID = ssid;
    pendingPass = pass;
    pendingTargetState = targetState;
    wifiConnectPhase = WIFI_CONNECTING;

    logEvent("SWITCH", "Connecting to: \"" + ssid + "\" (non-blocking, timeout: " + String(timeout / 1000) + "s)");
    WiFi.begin(ssid.c_str(), pass.c_str());
}

// Non-blocking WiFi connect poll
static void checkWiFiConnect() {
    if (wifiConnectPhase != WIFI_CONNECTING) return;

    if (WiFi.status() == WL_CONNECTED) {
        unsigned long duration = millis() - wifiConnectStartTime;
        logEvent("SWITCH", "Connected. Duration: " + String(duration) + " ms. IP: " + WiFi.localIP().toString());
        wifiConnectPhase = WIFI_IDLE;
        transitionTo(pendingTargetState, pendingSSID + " connected");
        return;
    }

    if (millis() - wifiConnectStartTime >= wifiConnectTimeout) {
        logEvent("ERROR", "Failed to connect to " + pendingSSID + " (timeout)");
        WiFi.disconnect(true);
        wifiConnectPhase = WIFI_IDLE;
        return;
    }
}

void initNetwork() {
    WiFi.mode(WIFI_STA);
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
}

void runWiFiStateMachine() {
    if (wifiConnectPhase == WIFI_CONNECTING) {
        checkWiFiConnect();
    }
}

void transitionTo(ProxyState newState, const String& reason) {
    if (newState == currentState) {
        return;
    }
    if (newState == STATE_SCANNING) {
        if (currentState != STATE_SCANNING) {
            String prevSSID = WiFi.SSID();
            if (prevSSID.length() == 0) {
                prevSSID = (currentState == STATE_CONNECTED_TO_WICAN) ? WICAN_SSID : "HomeNet";
            }
            logEvent("SWITCH", "Disconnecting from: \"" + prevSSID + "\"");
            if (reason.length() > 0) {
                logEvent("SWITCH", "Reason: " + reason);
            }

            // Log session stats upon disconnect
            if (obdSessionStartTime > 0) {
                unsigned long duration = (millis() - obdSessionStartTime) / 1000;
                logObdEvent("DISCONN", "OBD session closed. Duration: " + String(duration) + "s. Successfully read datasets: " + String(obdSessionSuccessCount) + ". Failed queries: " + String(obdSessionFailCount) + ".");
                obdSessionStartTime = 0;
            } else {
                logObdEvent("DISCONN", "OBD session closed (was not successfully established).");
            }

            disconnectOBD();
            WiFi.disconnect(true);
        }
        obdActive = false;
        isScanningActive = false;
        webServerRunning = false;
        mqttFlushDone = false;
        lastScanFailed = false;
        wifiConnectPhase = WIFI_IDLE;
    }
    else if (newState == STATE_CONNECTED_TO_WICAN) {
        extern unsigned long wicanConnectionTimer;
        extern unsigned long lastOBDReconnectAttempt;
        wicanConnectionTimer = millis();
        lastOBDReconnectAttempt = millis();

        // Clear cached data before new session
        memset(&latestCachedData, 0, sizeof(latestCachedData));
        strlcpy(latestCachedData.src, "CAR_BUFFERED", sizeof(latestCachedData.src));

        // Reset session statistics trackers
        obdSessionSuccessCount = 0;
        obdSessionFailCount = 0;
        obdSessionStartTime = millis();

        logObdEvent("CONN", "Connecting to OBD Gateway. Resetting session counters.");

        // Attempt TCP OBD socket connection
        obdActive = connectOBD();
        if (obdActive) {
            logObdEvent("CONN", "Connected to OBD Gateway successfully.");
        } else {
            logObdEvent("ERROR", "Initial OBD connection failed!");
            obdSessionFailCount++;
        }

        // Immediate pre-flight read cycle
        fetchOBDMetrics(true);

        lastTelemetryFetch = millis();
        lastSlowTelemetryFetch = millis();
    }
    else if (newState == STATE_CONNECTED_TO_HOME) {
        mqttFlushDone = false;
        lastHomeRescanCheck = millis();

        // Start NTP configuration with DST
        configTzTime(TZ_INFO, NTP_SERVER);

        // Start minimal background WebServer once
        webServerRunning = true;
        if (!isWebServerStarted) {
            server.begin();
            isWebServerStarted = true;
            logEvent("WEBSERVER", "Debug server started at http://" + WiFi.localIP().toString() + "/debug");
        }

        // Initialize OTA once
        if (!otaInitialized) {
            ArduinoOTA.setHostname("eup-proxy");
            ArduinoOTA.setPassword(OTA_PASSWORD);

            ArduinoOTA.onStart([]() {
                otaInProgress = true;
                Serial.println("[OTA] Update starting...");
            });
            ArduinoOTA.onEnd([]() {
                Serial.println("[OTA] Update complete. Rebooting...");
            });
            ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
                feedWDT();
            });
            ArduinoOTA.onError([](ota_error_t error) {
                otaInProgress = false;
                Serial.printf("[ERROR] OTA failed, error: %d\n", error);
            });

            ArduinoOTA.begin();
            otaInitialized = true;
            logEvent("OTA", "OTA service initialized on port 3232.");
        }
    }

    currentState = newState;
}

void handleScanning() {
    if (wifiConnectPhase == WIFI_CONNECTING) {
        checkWiFiConnect();
        return;
    }

    unsigned long currentMillis = millis();

    if (!isScanningActive) {
        if (!lastScanFailed) {
            logEvent("SCAN", "Starting non-blocking WiFi scan...");
        }
        WiFi.scanNetworks(true, false);
        scanStartTime = currentMillis;
        isScanningActive = true;
        return;
    }

    int scanResult = WiFi.scanComplete();

    if (scanResult >= 0) {
        bool wicanFound = false;
        bool home1Found = false;
        bool home2Found = false;
        int32_t wicanRSSI = -100;
        int32_t homeRSSI = -100;
        String homeSSID = "";
        String homePass = "";

        int knownCount = 0;
        if (scanResult > 0) {
            for (int i = 0; i < scanResult; i++) {
                String ssid = WiFi.SSID(i);
                if (ssid == WICAN_SSID || ssid == HOME_SSID_1 || ssid == HOME_SSID_2) {
                    knownCount++;
                }
            }
        }

        if (knownCount > 0) {
            lastScanFailed = false;
        }

        if (!lastScanFailed) {
            logEvent("SCAN", "Found " + String(knownCount) + " known networks:");
        }

        if (scanResult > 0) {
            int* indices = new int[scanResult];
            for (int i = 0; i < scanResult; i++) indices[i] = i;

            for (int i = 0; i < scanResult - 1; i++) {
                for (int j = i + 1; j < scanResult; j++) {
                    if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i])) {
                        int temp = indices[i];
                        indices[i] = indices[j];
                        indices[j] = temp;
                    }
                }
            }

            for (int k = 0; k < scanResult; k++) {
                int i = indices[k];
                String ssid = WiFi.SSID(i);
                int32_t rssi = WiFi.RSSI(i);
                int32_t channel = WiFi.channel(i);

                bool isKnown = (ssid == WICAN_SSID || ssid == HOME_SSID_1 || ssid == HOME_SSID_2);
                if (isKnown) {
                    if (!lastScanFailed) {
                        String quotedSSID = "\"" + ssid + "\"";
                        logEvent("SCAN", "  " + quotedSSID + "  RSSI: " + String(rssi) + " dBm  CH: " + String(channel) + "  [KNOWN]");
                    }

                    if (ssid == WICAN_SSID && !wicanFound) {
                        wicanFound = true;
                        wicanRSSI = rssi;
                    }
                    if (ssid == HOME_SSID_1 && !home1Found) {
                        home1Found = true;
                        if (rssi > homeRSSI) {
                            homeRSSI = rssi;
                            homeSSID = HOME_SSID_1;
                            homePass = HOME_PASS_1;
                        }
                    }
                    if (ssid == HOME_SSID_2 && !home2Found) {
                        home2Found = true;
                        if (rssi > homeRSSI) {
                            homeRSSI = rssi;
                            homeSSID = HOME_SSID_2;
                            homePass = HOME_PASS_2;
                        }
                    }
                }
            }
            delete[] indices;
        }

        isScanningActive = false;

        if (wicanFound) {
            logEvent("SCAN", "Priority target selected: \"" + String(WICAN_SSID) + "\"");
            WiFi.scanDelete();
            beginWiFiConnect(WICAN_SSID, WICAN_PASS, STATE_CONNECTED_TO_WICAN, WIFI_CONNECT_TIMEOUT_WICAN_MS);
        }
        else if (home1Found || home2Found) {
            logEvent("SCAN", "Priority target selected: \"" + homeSSID + "\"");
            WiFi.scanDelete();
            beginWiFiConnect(homeSSID, homePass, STATE_CONNECTED_TO_HOME, WIFI_CONNECT_TIMEOUT_HOME_MS);
        }
        else {
            WiFi.scanDelete();
            if (!lastScanFailed) {
                logEvent("SCAN", "No known networks found. Retrying in " + String(SCAN_RETRY_PAUSE_MS / 1000) + "s.");
                lastScanFailed = true;
            }
            scanStartTime = millis();
        }
    }
    else if (scanResult == WIFI_SCAN_FAILED) {
        if (!lastScanFailed) {
            logEvent("ERROR", "WiFi scan failed. Retrying...");
        }
        isScanningActive = false;
    }
    else if (currentMillis - scanStartTime > 15000) {
        if (!lastScanFailed) {
            logEvent("ERROR", "WiFi scan timed out! Restarting...");
        }
        WiFi.scanDelete();
        isScanningActive = false;
    }

    if (!isScanningActive && lastScanFailed && wifiConnectPhase == WIFI_IDLE) {
        if (millis() - scanStartTime < SCAN_RETRY_PAUSE_MS) {
            return;
        }
    }
}

void handleHome() {
    if (WiFi.status() != WL_CONNECTED) {
        transitionTo(STATE_SCANNING, "Home signal lost");
        return;
    }

    if (webServerRunning) {
        server.handleClient();
    }

    if (otaInitialized) {
        ArduinoOTA.handle();
    }

    if (!timeSyncDone) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 100)) {
            if (timeinfo.tm_year > 120) {
                char timeBuf[16];
                strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);

                char tzBuf[10];
                strftime(tzBuf, sizeof(tzBuf), "%Z", &timeinfo);

                char offBuf[10];
                strftime(offBuf, sizeof(offBuf), "%z", &timeinfo);
                String offStr = String(offBuf);
                if (offStr.length() == 5) {
                    offStr = offStr.substring(0, 3) + ":" + offStr.substring(3);
                }

                String localTimeMsg = "NTP synchronised. Local time: " + String(timeBuf) +
                                      " (Europe/Berlin, " + String(tzBuf) + " " + offStr + ")";

                logEvent("BOOT", localTimeMsg);
                timeSyncDone = true;
                g_ntpSynchronized = true;

                resetBootCrashCounter();
            }
        }
    }

    if (!mqttFlushDone) {
        flushQueueToMQTT();
    }

    unsigned long currentMillis = millis();

    if (!otaInProgress && currentMillis - lastHomeRescanCheck >= HOME_RESCAN_INTERVAL_MS) {
        logEvent("STATE", String(HOME_RESCAN_INTERVAL_MS / 60000) + "-minute home period elapsed. Checking if prioritized Wican is nearby...");
        transitionTo(STATE_SCANNING);
    }
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

    // Calculate the real-world boot epoch time:
    // current_synchronized_time - current_uptime_seconds
    time_t bootEpoch = time(nullptr) - (millis() / 1000);

    String filepath;
    TelemetryData data;
    int flushCount = 0;

    while (getNextQueuedFile(filepath, data)) {
        // Correct relative timestamps recorded before NTP was synchronized (below Jan 1, 2001)
        time_t correctedTs = data.ts;
        if (data.ts < 1000000000U) {
            correctedTs = bootEpoch + data.ts;
        }

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
        if (tzStr.length() == 5) {
            tzStr = tzStr.substring(0, 3) + ":" + tzStr.substring(3);
        }
        char tBuf[32];
        strftime(tBuf, sizeof(tBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
        snprintf(timeBuf, sizeof(timeBuf), "%s%s", tBuf, tzStr.c_str());
    }

    logMqttEvent("MQTT", "Updating eup/lastSync: " + String(timeBuf));
    mqttClient.publish(MQTT_TOPIC_LASTSYNC, timeBuf, true);

    mqttFlushDone = true;
}
