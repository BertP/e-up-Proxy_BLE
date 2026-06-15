#include <Arduino.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <Preferences.h>
#include "NetworkManager.h"
#include "OBDManager.h"
#include "config.h"
#include "logger.h"
#include "buffer.h"

Preferences prefs;
TelemetryData latestCachedData;
bool obdActive = false;
uint32_t obdSessionSuccessCount = 0;
uint32_t obdSessionFailCount = 0;
unsigned long obdSessionStartTime = 0;

static unsigned long lastLEDUpdate = 0;
unsigned long lastTelemetryFetch = 0;
unsigned long lastSlowTelemetryFetch = 0;


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
    unsigned long ledInterval = 500; 

    if (currentState == STATE_OPERATIONAL_ONLINE) {
        setLED(true);
        return;
    }

    if (currentState == STATE_SCANNING) {
        ledInterval = 100;
        if (currentMillis - lastLEDUpdate >= ledInterval) {
            lastLEDUpdate = currentMillis;
            digitalWrite(LED_PIN, !digitalRead(LED_PIN)); 
        }
    }
    else if (currentState == STATE_OPERATIONAL_BUFFERING) {
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

        if (groupAOk) {
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

    static unsigned long lastEmptyPayloadTime = 0;
    unsigned long now = millis();
    if (now - lastEmptyPayloadTime < 300000 && lastEmptyPayloadTime != 0) {
        logObdEvent("CONN", "OBD is offline, but empty payload was already generated recently. Skipping.");
        return;
    }
    lastEmptyPayloadTime = now;

    logObdEvent("CONN", "OBD is offline. Generating real board voltage empty payload...");
    generateEmptyRealTelemetry(latestCachedData);

    latestCachedData.ts = time(nullptr);
    strlcpy(latestCachedData.src, "CAR_BUFFERED", sizeof(latestCachedData.src));

    logTelemetry(latestCachedData);
    enqueueData(latestCachedData);
}

void setup() {
    Serial.begin(115200);
    delay(500);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    initNetwork();

    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);  
    pinMode(14, OUTPUT);
    digitalWrite(14, LOW); 

    pinMode(LED_PIN, OUTPUT);
    pinMode(IGNITION_PIN, INPUT_PULLUP);
    setLED(false);

    initLogger();
    checkFirmwareUpdate();
    initBuffer();

    logBootSequence(WiFi.macAddress(), 50, getQueueSize());
    checkBootLoopProtection();
    
    // External function declared somewhere for diagnostics server
    extern void initDiagnosticsServer();
    initDiagnosticsServer();

    setupWDT();
    transitionTo(STATE_SCANNING);
}

void loop() {
    feedWDT();

    // Ignition OFF detection (skip first 10s after boot to allow USB debugging)
    if (millis() > 10000) {
        if (digitalRead(IGNITION_PIN) == LOW) {
            delay(100); // Debounce
            if (digitalRead(IGNITION_PIN) == LOW) {
                initiateShutdownSequence();
            }
        }
    }

    updateLED();

    if (!stateMachineInitLogged && millis() > 10000) {
        logEvent("BOOT", "State machine initialised.");
        stateMachineInitLogged = true;
    }

    runNetworkStateMachine();

    static unsigned long lastAlive = 0;
    if (millis() - lastAlive > 60000) {
        lastAlive = millis();
        logEvent("DEBUG", "System alive. Uptime: " + String(millis() / 1000) + "s, Free heap: " + String(ESP.getFreeHeap()) + "B");
    }

    delay(1);
}
