#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "DiagnosticsServer.h"
#include "config.h"
#include "logger.h"
#include "buffer.h"
#include "OBDManager.h"
#include "NetworkManager.h"

// Define global server instance
WebServer server(80);

extern TelemetryData latestCachedData;
extern bool obdActive;

void initDiagnosticsServer() {
    // Register /debug endpoint
    server.on("/debug", HTTP_GET, []() {
        logEvent("WEBSERVER", "GET /debug endpoint requested.");
        streamLog(server);
    });

    // Register /mqtt endpoint
    server.on("/mqtt", HTTP_GET, []() {
        logMqttEvent("WEBSERVER", "GET /mqtt endpoint requested.");
        streamMqttLog(server);
    });

    // Register /obd endpoint
    server.on("/obd", HTTP_GET, []() {
        logObdEvent("WEBSERVER", "GET /obd endpoint requested.");
        streamObdLog(server);
    });

    // Register active REST API /files endpoint
    server.on("/files", HTTP_GET, []() {
        logEvent("WEBSERVER", "GET /files endpoint requested.");
        JsonDocument doc;
        JsonArray arr = doc["files"].to<JsonArray>();
        File dir = LittleFS.open("/queue", "r");
        if (dir && dir.isDirectory()) {
            File file = dir.openNextFile();
            while (file) {
                if (!file.isDirectory()) {
                    JsonObject f = arr.add<JsonObject>();
                    f["name"] = String(file.name());
                    f["size"] = file.size();
                }
                file = dir.openNextFile();
            }
            dir.close();
        }
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    // Register active REST API /clear endpoint
    server.on("/clear", HTTP_POST, []() {
        logEvent("WEBSERVER", "POST /clear endpoint requested.");
        clearQueue();
        server.send(200, "text/plain", "Queue cleared successfully");
    });

    // Register health and metrics /status endpoint
    server.on("/status", HTTP_GET, []() {
        logEvent("WEBSERVER", "GET /status endpoint requested.");
        JsonDocument doc;
        doc["uptime_s"] = millis() / 1000;
        doc["state"] = stateNames[currentState];
        doc["free_heap"] = ESP.getFreeHeap();
        doc["wifi_rssi"] = WiFi.RSSI();
        doc["wifi_ssid"] = WiFi.SSID();
        doc["mqtt_connected"] = mqttClient.connected();
        doc["mqtt_state"] = mqttClient.state();
        doc["obd_active"] = obdActive;
        doc["queue_size"] = getQueueSize();
        doc["fw_version"] = FW_VERSION;
        doc["hardware"] = HARDWARE_PLATFORM;

        JsonObject obd = doc["latest_obd"].to<JsonObject>();
        obd["soc"] = std::isnan(latestCachedData.soc) ? JsonVariant() : latestCachedData.soc;
        obd["volt"] = std::isnan(latestCachedData.volt) ? JsonVariant() : latestCachedData.volt;
        obd["temp"] = std::isnan(latestCachedData.temp) ? JsonVariant() : latestCachedData.temp;
        obd["bat_cap"] = std::isnan(latestCachedData.bat_cap) ? JsonVariant() : latestCachedData.bat_cap;
        obd["odo"] = std::isnan(latestCachedData.odo) ? JsonVariant() : latestCachedData.odo;

        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });
}
