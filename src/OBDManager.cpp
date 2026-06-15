#include "OBDManager.h"
#include "config.h"
#include "logger.h"
#include <vector>
#include <esp_task_wdt.h>

static NimBLEClient* pBleClient = nullptr;
static NimBLERemoteCharacteristic* pWriteChar = nullptr;
static NimBLERemoteCharacteristic* pNotifyChar = nullptr;

static String currentHeader = "";
static unsigned long lastTesterPresent = 0;
static float lastRealVoltage = 12.0f;
extern void feedWDT(); // Keep watchdog alive

static volatile bool responseReady = false;
static String bleResponse = "";

volatile bool isAuthenticationComplete = false;
volatile bool isAuthenticationFailed = false;

extern bool obdActive;

class MyClientCallback : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override {
        logEvent("CONN", "NimBLEClientCallbacks: Connected");
    }

    void onDisconnect(NimBLEClient* pClient, int reason) override {
        logEvent("DISCONN", "NimBLE Client Disconnected. Reason: " + String(reason));
        obdActive = false;
    }
    
    void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
        logEvent("SEC", "Passkey requested, injecting 654321");
        NimBLEDevice::injectPassKey(connInfo, 654321);
    }
    
    void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pass_key) override {
        logEvent("SEC", "Confirming Passkey: " + String(pass_key));
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }
    
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (!connInfo.isEncrypted()) {
            logEvent("ERROR", "Encrypt connection failed - disconnecting");
            isAuthenticationFailed = true;
            NimBLEDevice::getClientByHandle(connInfo.getConnHandle())->disconnect();
        } else {
            logEvent("SEC", "Authentication complete. Connection is encrypted.");
            isAuthenticationComplete = true;
        }
    }
};



static void notifyCallback(NimBLERemoteCharacteristic* pNimBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
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
        if (header == "7E5") {
            sendCommand("AT CRA 7ED");
        } else if (header == "7E0") {
            sendCommand("AT CRA 7E8");
        } else {
            sendCommand("AT AR"); // Auto receive filter for other ECUs
        }
        return true;
    }

    logObdEvent("ERROR", "Failed to switch header to " + header + " - Response: " + resp);
    return false;
}

bool connectOBD(NimBLEAdvertisedDevice* device) {
    if (!device) return false;

    logObdEvent("CONN", "Connecting to OBD NimBLE Gateway: " + String(device->getName().c_str()));
    
    if (pBleClient == nullptr) {
        pBleClient = NimBLEDevice::createClient();
        pBleClient->setClientCallbacks(new MyClientCallback());
        
        // Use robust connection parameters from NimBLE examples
        pBleClient->setConnectionParams(12, 12, 0, 150);
        pBleClient->setConnectTimeout(5 * 1000);
        
        // Setup Security as per WiCAN docs
        NimBLEDevice::setSecurityAuth(true, true, true);
    }

    logEvent("CONN", "Connecting to OBD NimBLE Gateway: " + String(device->getName().c_str()));
    if (!pBleClient->connect(device)) {
        logEvent("ERROR", "NimBLE connect failed");
        return false;
    }

    isAuthenticationComplete = false;
    isAuthenticationFailed = false;

    logEvent("CONN", "Waiting up to 5s for Security Handshake...");
    for (int i = 0; i < 50; i++) {
        if (isAuthenticationComplete || isAuthenticationFailed) {
            break;
        }
        delay(100);
    }

    if (isAuthenticationFailed) {
        logEvent("ERROR", "Authentication failed, aborting connection.");
        pBleClient->disconnect();
        return false;
    }

    logEvent("CONN", "Finding Service...");
    // Disable Task WDT because GATT discovery on WiCAN takes > 5 seconds and blocks the loop
    esp_task_wdt_delete(NULL);

    // Print all discovered services to see what the WiCAN is actually serving
    std::vector<NimBLERemoteService*> services = pBleClient->getServices(true);
    for (auto pSvc : services) {
        logEvent("DEBUG", "Found Service: " + String(pSvc->getUUID().toString().c_str()));
    }

    // Fetch the standard OBD Service FFF0
    NimBLERemoteService* pRemoteService = pBleClient->getService(NimBLEUUID("FFF0"));
    
    if (pRemoteService == nullptr) {
        logEvent("ERROR", "Failed to find FFF0 Service.");
        esp_task_wdt_add(NULL); // Re-enable before returning
        pBleClient->disconnect();
        return false;
    }
    
    logEvent("DEBUG", "Successfully found FFF0 Service!");

    // Print all characteristics of FFF0
    std::vector<NimBLERemoteCharacteristic*> chars = pRemoteService->getCharacteristics(true);
    for (auto pChar : chars) {
        logEvent("DEBUG", "Found Char: " + String(pChar->getUUID().toString().c_str()));
    }

    esp_task_wdt_add(NULL); // Re-enable WDT after full discovery

    // Dynamically find TX/RX characteristics based on WiCAN spec
    pWriteChar = pRemoteService->getCharacteristic(NimBLEUUID("FFF2"));
    pNotifyChar = pRemoteService->getCharacteristic(NimBLEUUID("FFF1"));

    if (pWriteChar == nullptr || pNotifyChar == nullptr) {
        logEvent("ERROR", "Failed to find Nordic UART Characteristics.");
        pBleClient->disconnect();
        return false;
    }

    logEvent("CONN", "Found RX/TX chars. Registering notification...");
    
    if (pNotifyChar->canNotify()) {
        pNotifyChar->subscribe(true, notifyCallback);
    } else if (pNotifyChar->canIndicate()) {
        pNotifyChar->subscribe(false, notifyCallback);
    }

    logEvent("CONN", "Characteristics found and notifications registered. Running ELM327 init...");
    
    delay(500);
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
            
            // Also try to open session on 7E0 and 714
            if (setHeader("7E0")) {
                sendCommand("10 03");
            }
            if (setHeader("714")) {
                sendCommand("10 03");
            }
            
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
        logObdEvent("CONN", "OBD session closed. NimBLE disconnected.");
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
            sendCommand("3E 00");
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
    if (std::isnan(data.soc)) {
        data.range = NAN;
        return;
    }
    
    // Fallback capacity to 61.5 Ah (approx 32.3 kWh net) if the ECU 7E5 rejects the bat_cap query
    float cap = std::isnan(data.bat_cap) ? 61.5f : data.bat_cap;

    float range_coefficient = 5.5f; // Standard warm temperature coefficient
    if (!std::isnan(data.temp)) {
        if (data.temp <= 0.0f) {
            range_coefficient = 3.5f; // Frozen battery coefficient
        } else if (data.temp < 15.0f) {
            range_coefficient = 4.5f; // Cold temperature coefficient
        }
    }
    data.range = cap * (data.soc / 100.0f) * range_coefficient;
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
    // Instrument Cluster (714) using standard VW ODO DID 02 BD
    if (queryUDS3Bytes("714", "02 BD", odoVal, 1.0f, 0.0f)) {
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
