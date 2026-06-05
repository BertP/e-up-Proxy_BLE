#include <Arduino.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <LittleFS.h>
#include <cmath>

#include "config.h"
#include "logger.h"
#include "buffer.h"
#include "OBDManager.h"
#include "NetworkManager.h"
#include "DiagnosticsServer.h"

// Global instances and shared state variables
Preferences prefs;
TelemetryData latestCachedData;
bool obdActive = false;

// OBD Session Stats Tracker
uint32_t obdSessionSuccessCount = 0;
uint32_t obdSessionFailCount = 0;
unsigned long obdSessionStartTime = 0;

// Timing variables (non-blocking)
unsigned long lastStateChange = 0;
static unsigned long lastLEDUpdate = 0;
unsigned long lastTelemetryFetch = 0;
unsigned long lastSlowTelemetryFetch = 0;

unsigned long wicanConnectionTimer = 0;
unsigned long lastOBDReconnectAttempt = 0;

static bool stateMachineInitLogged = false;

// Function declarations
void setupWDT();
void feedWDT();
void checkBootLoopProtection();
void resetBootCrashCounter();
void updateLED();
void fetchOBDMetrics(bool forceSlow);
void generateEmptyRealTelemetry(TelemetryData& data);

void setupWDT() {
    logEvent("BOOT", "Initializing hardware Watchdog (" + String(WDT_TIMEOUT_S) + "s)...");
    #if ESP_ARDUINO_VERSION_MAJOR >= 3
        esp_task_wdt_config_t wdt_config = {
            .timeout_ms = WDT_TIMEOUT_S * 1000,
            .idle_core_mask = 0,
            .trigger_panic = true
        };
        esp_task_wdt_init(&wdt_config);
        esp_task_wdt_add(NULL);
    #else
        esp_task_wdt_init(WDT_TIMEOUT_S, true);
        esp_task_wdt_add(NULL);
    #endif
}

void feedWDT() {
    esp_task_wdt_reset();
}

void checkBootLoopProtection() {
    prefs.begin("proxy", false);
    int crashCount = prefs.getInt(BOOT_CRASH_NVS_KEY, 0);

    if (crashCount >= BOOT_CRASH_THRESHOLD) {
        logEvent("BOOT", "WARNING: Boot-loop detected (" + String(crashCount) + " consecutive crashes). Resetting counter.");
        prefs.putInt(BOOT_CRASH_NVS_KEY, 0);
    } else {
        prefs.putInt(BOOT_CRASH_NVS_KEY, crashCount + 1);
        logEvent("BOOT", "Boot counter: " + String(crashCount + 1) + "/" + String(BOOT_CRASH_THRESHOLD));
    }
    prefs.end();
}

void resetBootCrashCounter() {
    prefs.begin("proxy", false);
    prefs.putInt(BOOT_CRASH_NVS_KEY, 0);
    prefs.end();
}

void checkFirmwareUpdate() {
    prefs.begin("proxy", false);
    String lastFw = prefs.getString("fw_version", "");
    if (lastFw != FW_VERSION) {
        clearLog();
        logEvent("BOOT", "Firmware version changed from '" + lastFw + "' to '" + String(FW_VERSION) + "'. Log files cleared.");
        prefs.putString("fw_version", FW_VERSION);
    }
    prefs.end();
}


void setLED(bool on) {
#if defined(LED_ACTIVE_HIGH) && LED_ACTIVE_HIGH == 0
    digitalWrite(LED_PIN, on ? LOW : HIGH);
#else
    digitalWrite(LED_PIN, on ? HIGH : LOW);
#endif
}

void updateLED() {
    unsigned long currentMillis = millis();
    unsigned long ledInterval = 500; // Default scanning: 500ms toggle (actually we specify scanning below)

    if (currentState == STATE_CONNECTED_TO_HOME) {
        // Solid ON (or slow pulse)
        setLED(true);
        return;
    }

    if (currentState == STATE_SCANNING) {
        // Fast blinking: 100ms ON / 100ms OFF
        ledInterval = 100;
        if (currentMillis - lastLEDUpdate >= ledInterval) {
            lastLEDUpdate = currentMillis;
            digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Toggling directly flips the state regardless of polarity
        }
    }
    else if (currentState == STATE_CONNECTED_TO_WICAN) {
        // Distinct double-blink: 2 quick blinks every 2 seconds
        unsigned long cycleTime = currentMillis % 2000;
        if (cycleTime < 100) {
            setLED(true);
        } else if (cycleTime < 250) {
            setLED(false);
        } else if (cycleTime < 350) {
            setLED(true);
        } else {
            setLED(false);
        }
    }
}

void generateEmptyRealTelemetry(TelemetryData& data) {
    data.soc = NAN;
    data.volt = getLatestRealVoltage();
    data.temp = NAN;
    data.range = NAN;
    data.power = NAN;
    data.bat_cap = NAN;
    data.tp_alarm = NAN;
    data.odo = NAN;
    data.service_days = NAN;
    data.service_km = NAN;
}

void fetchOBDMetrics(bool forceSlow) {
    if (obdActive) {
        bool groupAOk = queryGroupA(latestCachedData);
        bool groupBOk = true;

        if (forceSlow) {
            groupBOk = queryGroupB(latestCachedData);
        }

        if (groupAOk && groupBOk) {
            latestCachedData.ts = time(nullptr);
            strlcpy(latestCachedData.src, "CAR_BUFFERED", sizeof(latestCachedData.src));

            logTelemetry(latestCachedData);
            if (forceSlow) {
                logTelemetrySlow(latestCachedData);
            }

            enqueueData(latestCachedData);
            obdSessionSuccessCount++;
            return;
        } else {
            logObdEvent("ERROR", "UDS OBD queries failed. Disconnecting OBD for active retry.");
            obdSessionFailCount++;
            disconnectOBD();
            obdActive = false;
        }
    }

    logObdEvent("CONN", "OBD is offline. Generating real board voltage empty payload...");
    generateEmptyRealTelemetry(latestCachedData);

    latestCachedData.ts = time(nullptr);
    strlcpy(latestCachedData.src, "CAR_BUFFERED", sizeof(latestCachedData.src));

    logTelemetry(latestCachedData);

    enqueueData(latestCachedData);
}

void handleWican() {
    if (WiFi.status() != WL_CONNECTED) {
        transitionTo(STATE_SCANNING, "Wican signal lost");
        return;
    }

    unsigned long currentMillis = millis();

    if (obdActive) {
        runOBDKeepAlive();
    } else {
        // Active TCP Reconnection scheduler
        if (currentMillis - lastOBDReconnectAttempt >= OBD_RECONNECT_INTERVAL_MS) {
            lastOBDReconnectAttempt = currentMillis;
            logEvent("CONN", "OBD is offline. Attempting TCP reconnection...");
            obdActive = connectOBD();
            if (obdActive) {
                logEvent("CONN", "OBD TCP connection re-established successfully.");
                fetchOBDMetrics(true);
                lastTelemetryFetch = currentMillis;
                lastSlowTelemetryFetch = currentMillis;
            }
        }

        // Timeout if no OBD connection established
        if (currentMillis - wicanConnectionTimer >= WICAN_TIMEOUT_MS) {
            transitionTo(STATE_SCANNING, "Wican timeout");
            return;
        }
    }

    if (currentMillis - lastSlowTelemetryFetch >= POLL_INTERVAL_SLOW_MS) {
        lastSlowTelemetryFetch = currentMillis;
        fetchOBDMetrics(true);
        lastTelemetryFetch = currentMillis;
    }
    else if (currentMillis - lastTelemetryFetch >= POLL_INTERVAL_FAST_MS) {
        lastTelemetryFetch = currentMillis;
        fetchOBDMetrics(false);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    // Initialize low-level NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Initialize Network Managers
    initNetwork();

    // Seeed Studio XIAO ESP32-C6 RF Switch and Ceramic Antenna activation
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);  // Activate RF switch control
    pinMode(14, OUTPUT);
    digitalWrite(14, LOW); // Select built-in ceramic antenna

    pinMode(LED_PIN, OUTPUT);
    setLED(false);

    initLogger();

    // Check for firmware update and clear logs if updated
    checkFirmwareUpdate();

    initBuffer();

    logBootSequence(WiFi.macAddress(), 50, getQueueSize());

    // Boot-loop protection check
    checkBootLoopProtection();

    // Initialize REST WebServer route endpoints
    initDiagnosticsServer();

    setupWDT();

    transitionTo(STATE_SCANNING);
}

void loop() {
    feedWDT();
    updateLED();

    if (!stateMachineInitLogged && millis() > 10000) {
        logEvent("BOOT", "State machine initialised. Entering SCANNING.");
        stateMachineInitLogged = true;
    }

    runWiFiStateMachine();

    switch (currentState) {
        case STATE_SCANNING:
            handleScanning();
            break;
        case STATE_CONNECTED_TO_WICAN:
            handleWican();
            break;
        case STATE_CONNECTED_TO_HOME:
            handleHome();
            break;
    }

    delay(1);
}
