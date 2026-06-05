#include "OBDManager.h"
#include "config.h"
#include "logger.h"

static BLEClient* pBleClient = nullptr;
static BLERemoteCharacteristic* pWriteChar = nullptr;
static BLERemoteCharacteristic* pNotifyChar = nullptr;

static String currentHeader = "";
static unsigned long lastTesterPresent = 0;
static float lastRealVoltage = 12.0f;
extern void feedWDT(); // Keep watchdog alive

static volatile bool responseReady = false;
static String bleResponse = "";

class MyClientCallback : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) {
    }

    void onDisconnect(BLEClient* pclient) {
        logObdEvent("DISCONN", "BLE Client Disconnected");
    }
};

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    for (size_t i = 0; i < length; i++) {
        char c = (char)pData[i];
        if (c == '>') {
            responseReady = true;
        } else {
            bleResponse += c;
        }
    }
}

static String stripWhitespace(const String& str) {
    String clean;
    clean.reserve(str.length());
    for (unsigned int i = 0; i < str.length(); i++) {
        char c = str[i];
        if (c != ' ' && c != '\r' && c != '\n') {
            clean += c;
        }
    }
    return clean;
}

static String sendCommand(const String& cmd, unsigned int timeout = OBD_CMD_TIMEOUT_MS) {
    if (!pBleClient || !pBleClient->isConnected() || !pWriteChar) {
        return "";
    }

    responseReady = false;
    bleResponse = "";

    String fullCmd = cmd + "\r";
    pWriteChar->writeValue(fullCmd.c_str(), fullCmd.length());

    unsigned long start = millis();
    bool timedOut = true;
    while (millis() - start < timeout) {
        if (responseReady) {
            timedOut = false;
            break;
        }
        delay(10);
        feedWDT();
    }
    
    String response = bleResponse;
    response.trim();

    if (timedOut && response.length() == 0) {
        logObdEvent("OBD-TIMEOUT", "No response for command: " + cmd + " (timeout: " + String(timeout) + "ms)");
    } else if (timedOut) {
        logObdEvent("OBD-TIMEOUT", "Partial response for command: " + cmd + " -> \"" + response + "\" (timeout: " + String(timeout) + "ms)");
    }

    return response;
}

static bool setHeader(const String& header) {
    if (currentHeader == header) {
        return true;
    }

    logObdEvent("CONN", "Switching CAN Header to " + header);
    String resp = sendCommand("AT SH " + header);
    String clean = stripWhitespace(resp);

    if (clean.equalsIgnoreCase("OK") || clean.indexOf("OK") >= 0 || clean.length() == 0) {
        currentHeader = header;
        return true;
    }

    logObdEvent("ERROR", "Failed to switch header to " + header + " - Response: " + resp);
    return false;
}

bool connectOBD(BLEAdvertisedDevice* device) {
    if (!device) return false;

    logObdEvent("CONN", "Connecting to OBD BLE Gateway: " + String(device->getName().c_str()));
    
    if (pBleClient == nullptr) {
        pBleClient = BLEDevice::createClient();
        pBleClient->setClientCallbacks(new MyClientCallback());
    }

    if (!pBleClient->connect(device)) {
        logObdEvent("ERROR", "OBD BLE connection failed!");
        return false;
    }

    logObdEvent("CONN", "BLE Connected. Finding Service...");
    
    BLERemoteService* pRemoteService = pBleClient->getService(BLEUUID(WICAN_BLE_SERVICE_UUID));
    if (pRemoteService == nullptr) {
        logObdEvent("ERROR", "Failed to find FFF0 service.");
        pBleClient->disconnect();
        return false;
    }

    // Dynamically find TX/RX characteristics
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    std::map<uint16_t, BLERemoteCharacteristic*>* pCharacteristics = pRemoteService->getCharacteristicsByHandle();
    for (auto& elem : *pCharacteristics) {
        BLERemoteCharacteristic* pChar = elem.second;
        if (pChar->canWrite() || pChar->canWriteNoResponse()) pWriteChar = pChar;
        if (pChar->canNotify()) pNotifyChar = pChar;
    }

    if (!pWriteChar || !pNotifyChar) {
        logObdEvent("ERROR", "Failed to find necessary characteristics.");
        pBleClient->disconnect();
        return false;
    }

    pNotifyChar->registerForNotify(notifyCallback);
    logObdEvent("CONN", "Characteristics found and notifications registered. Running ELM327 init...");

    currentHeader = ""; // Reset cached header

    // ELM327 Init Sequence
    const char* initCmds[] = { "AT E0", "AT L0", "AT H0", "AT SP 6", "AT AT 1", "AT ST FF" };
    for (const char* cmd : initCmds) {
        String resp = sendCommand(cmd);
        logObdEvent("CONN", "ELM327 Init: " + String(cmd) + " -> " + resp);
    }

    // Measure real board voltage directly from ELM327 (works even when CAN is offline)
    String rvResp = sendCommand("AT RV");
    String cleanRv = stripWhitespace(rvResp);
    if (cleanRv.length() > 0) {
        float voltVal = cleanRv.toFloat();
        if (voltVal > 0.0f) {
            lastRealVoltage = voltVal;
            logObdEvent("CONN", "Real OBD board voltage read: " + String(voltVal) + "V");
        }
    }

    // Open UDS Extended Diagnostic Session on ECU 7E5
    if (setHeader("7E5")) {
        logObdEvent("CONN", "Opening UDS extended diagnostic session (10 03) on 7E5...");
        String resp = sendCommand("10 03");
        String clean = stripWhitespace(resp);
        if (clean.indexOf("5003") >= 0) {
            logObdEvent("CONN", "UDS extended session opened on 7E5.");
            lastTesterPresent = millis();
            return true;
        } else {
            logObdEvent("ERROR", "Failed to open UDS extended session! Response: " + resp);
        }
    }

    disconnectOBD();
    return false;
}

void disconnectOBD() {
    if (pBleClient && pBleClient->isConnected()) {
        logObdEvent("CONN", "OBD session closed. BLE disconnected.");
        pBleClient->disconnect();
    }
}

void runOBDKeepAlive() {
    if (!pBleClient || !pBleClient->isConnected()) {
        return;
    }

    unsigned long now = millis();
    if (now - lastTesterPresent >= TESTER_PRESENT_INTERVAL_MS) {
        lastTesterPresent = now;
        String savedHeader = currentHeader;
        if (setHeader("7E5")) {
            sendCommand("3E 80", 500);
        }
        if (savedHeader.length() > 0 && savedHeader != "7E5") {
            setHeader(savedHeader);
        }
    }
}

static bool queryUDS1Byte(const String& header, const String& did, float& outVal, float scale, float offset) {
    if (!setHeader(header)) return false;

    String cmd = "22 " + did;
    String resp = sendCommand(cmd);
    String clean = stripWhitespace(resp);

    String expectedPrefix = "62" + stripWhitespace(did);
    int prefixIndex = clean.indexOf(expectedPrefix);

    if (prefixIndex >= 0 && clean.length() >= prefixIndex + expectedPrefix.length() + 2) {
        String hexByte = clean.substring(prefixIndex + expectedPrefix.length(), prefixIndex + expectedPrefix.length() + 2);
        long rawVal = strtol(hexByte.c_str(), NULL, 16);
        outVal = (rawVal * scale) + offset;
        return true;
    }

    if (clean.length() == 0) {
        logObdEvent("ERROR", "OBD query failed: DID " + did + " — no response (timeout)");
    } else if (clean.indexOf("7F") >= 0) {
        logObdEvent("ERROR", "OBD query failed: DID " + did + " — NRC response: \"" + resp + "\"");
    } else {
        logObdEvent("ERROR", "OBD query failed: DID " + did + " — unexpected response: \"" + resp + "\"");
    }
    return false;
}

static bool queryUDS2Bytes(const String& header, const String& did, float& outVal, float scale, float offset) {
    if (!setHeader(header)) return false;

    String cmd = "22 " + did;
    String resp = sendCommand(cmd);
    String clean = stripWhitespace(resp);

    String expectedPrefix = "62" + stripWhitespace(did);
    int prefixIndex = clean.indexOf(expectedPrefix);

    if (prefixIndex >= 0 && clean.length() >= prefixIndex + expectedPrefix.length() + 4) {
        String hexBytes = clean.substring(prefixIndex + expectedPrefix.length(), prefixIndex + expectedPrefix.length() + 4);
        long rawVal = strtol(hexBytes.c_str(), NULL, 16);
        int16_t signedVal = (int16_t)rawVal;
        outVal = (signedVal * scale) + offset;
        return true;
    }

    if (clean.length() == 0) {
        logObdEvent("ERROR", "OBD query failed: DID " + did + " — no response (timeout)");
    } else if (clean.indexOf("7F") >= 0) {
        logObdEvent("ERROR", "OBD query failed: DID " + did + " — NRC response: \"" + resp + "\"");
    } else {
        logObdEvent("ERROR", "OBD query failed: DID " + did + " — unexpected response: \"" + resp + "\"");
    }
    return false;
}

static bool queryUDS3Bytes(const String& header, const String& did, float& outVal, float scale, float offset) {
    if (!setHeader(header)) return false;

    String cmd = "22 " + did;
    String resp = sendCommand(cmd);
    String clean = stripWhitespace(resp);

    String expectedPrefix = "62" + stripWhitespace(did);
    int prefixIndex = clean.indexOf(expectedPrefix);

    if (prefixIndex >= 0 && clean.length() >= prefixIndex + expectedPrefix.length() + 6) {
        String hexBytes = clean.substring(prefixIndex + expectedPrefix.length(), prefixIndex + expectedPrefix.length() + 6);
        long rawVal = strtol(hexBytes.c_str(), NULL, 16);
        outVal = (rawVal * scale) + offset;
        return true;
    }

    if (clean.length() == 0) {
        logObdEvent("ERROR", "OBD query failed: DID " + did + " — no response (timeout)");
    } else if (clean.indexOf("7F") >= 0) {
        logObdEvent("ERROR", "OBD query failed: DID " + did + " — NRC response: \"" + resp + "\"");
    } else {
        logObdEvent("ERROR", "OBD query failed: DID " + did + " — unexpected response: \"" + resp + "\"");
    }
    return false;
}

static void deriveRange(TelemetryData& data) {
    if (std::isnan(data.bat_cap) || std::isnan(data.soc)) {
        data.range = NAN;
        return;
    }
    float range_coefficient = 5.5f; // Standard warm temperature coefficient
    if (!std::isnan(data.temp)) {
        if (data.temp <= 0.0f) {
            range_coefficient = 3.5f; // Frozen battery coefficient
        } else if (data.temp < 15.0f) {
            range_coefficient = 4.5f; // Cold temperature coefficient
        }
    }
    data.range = data.bat_cap * (data.soc / 100.0f) * range_coefficient;
}

bool queryGroupA(TelemetryData& data) {
    if (!pBleClient || !pBleClient->isConnected()) return false;

    bool hasSomeData = false;

    float socVal = 0.0f;
    if (queryUDS1Byte("7E5", "02 8C", socVal, 0.4f, 0.0f)) {
        data.soc = socVal;
        hasSomeData = true;
    } else {
        data.soc = NAN;
    }

    float tempVal = 0.0f;
    if (queryUDS2Bytes("7E5", "2A 0B", tempVal, 0.015625f, 0.0f)) {
        data.temp = tempVal;
        hasSomeData = true;
    } else {
        data.temp = NAN;
    }

    float capVal = 0.0f;
    if (queryUDS2Bytes("7E5", "22 E1", capVal, 0.1f, 0.0f)) {
        data.bat_cap = capVal;
        hasSomeData = true;
    } else {
        data.bat_cap = NAN;
    }

    String rvResp = sendCommand("AT RV");
    String cleanRv = stripWhitespace(rvResp);
    float voltVal = 0.0f;
    if (cleanRv.length() > 0) {
        voltVal = cleanRv.toFloat();
        if (voltVal > 0.0f) {
            data.volt = voltVal;
            lastRealVoltage = voltVal; 
            hasSomeData = true;
        } else {
            data.volt = NAN;
            logEvent("ERROR", "OBD query failed: voltage (AT RV) — raw response: \"" + rvResp + "\"");
        }
    } else {
        data.volt = NAN;
    }

    float tpVal = 0.0f;
    if (queryUDS1Byte("7E0", "02 1A", tpVal, 1.0f, 0.0f)) {
        data.tp_alarm = tpVal;
        hasSomeData = true;
    } else {
        data.tp_alarm = NAN;
    }

    deriveRange(data);
    data.power = NAN; 

    return hasSomeData;
}

bool queryGroupB(TelemetryData& data) {
    if (!pBleClient || !pBleClient->isConnected()) return false;

    bool hasSomeData = false;

    float odoVal = 0.0f;
    if (queryUDS3Bytes("7E0", "22 03", odoVal, 1.0f, 0.0f)) {
        data.odo = odoVal;
        hasSomeData = true;
    } else {
        data.odo = NAN;
    }

    float daysVal = 0.0f;
    if (queryUDS2Bytes("7E0", "02 48", daysVal, 1.0f, 0.0f)) {
        data.service_days = daysVal;
        hasSomeData = true;
    } else {
        data.service_days = NAN;
    }

    float kmVal = 0.0f;
    if (queryUDS2Bytes("7E0", "02 47", kmVal, 1.0f, 0.0f)) {
        data.service_km = kmVal;
        hasSomeData = true;
    } else {
        data.service_km = NAN;
    }

    return hasSomeData;
}

void generateSimulatedTelemetry(TelemetryData& data, bool slowGroup) {
    if (!slowGroup) {
        data.soc = 82.5f;
        data.volt = 12.4f;
        data.temp = 18.0f;
        data.bat_cap = 61.5f;
        data.tp_alarm = 0.0f;

        deriveRange(data);
        data.power = -2100.0f;
    } else {
        data.odo = 42150.0f;
        data.service_days = 180.0f;
        data.service_km = 7500.0f;
    }
}

float getLatestRealVoltage() {
    return lastRealVoltage;
}
