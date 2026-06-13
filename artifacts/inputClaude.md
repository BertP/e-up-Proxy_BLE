# WiCAN Pro — BLE Interface Dokumentation

**Ziel:** ESP32-C6 als BLE-Client verbindet sich zum WiCAN Pro, um Fahrzeugdaten (e-up!) via ELM327-Protokoll abzufragen.

---

## GATT Services & Characteristics

Der WiCAN Pro exponiert über BLE einen **Nordic UART Service (NUS)** — den de-facto-Standard für serielle Kommunikation über BLE auf ESP32-Basis.

| Rolle | UUID |
|---|---|
| **Service** (Nordic UART Service) | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| **RX Characteristic** (Write → Dongle) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| **TX Characteristic** (Notify ← Dongle) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

Zusätzlich Standard-GAP-Services:

| Service | UUID |
|---|---|
| Generic Access | `0x1800` |
| Generic Attribute | `0x1801` |

---

## Kommunikationsprotokoll

Über NUS läuft **ELM327-Emulation** — Befehle und Antworten als ASCII-Text mit `\r` als Terminator.

### Typische Initialisierungssequenz

```
ATZ\r          → Reset (Antwort: "ELM327 v1.5" o.ä.)
ATE0\r         → Echo deaktivieren
ATH1\r         → Header in Antworten einblenden
ATSP0\r        → OBD-Protokoll automatisch erkennen
```

### Standard OBD-II PID Beispiele

```
0100\r         → Unterstützte PIDs (01-20) abfragen
0105\r         → Kühlmitteltemperatur
010C\r         → Motordrehzahl (RPM)
010D\r         → Fahrzeuggeschwindigkeit
```

> **Hinweis:** Herstellerspezifische PIDs für den VW e-up! liegen außerhalb des Standard-OBD-II-Bereichs und müssen separat recherchiert/gescannt werden.

---

## NimBLE 2.x Code (ESP32-C6)

### Konstanten

```cpp
#define WICAN_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define WICAN_CHAR_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // Write → Dongle
#define WICAN_CHAR_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // Notify ← Dongle
```

### Vollständiges Grundgerüst

```cpp
#include <NimBLEDevice.h>

#define WICAN_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define WICAN_CHAR_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define WICAN_CHAR_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define WICAN_DEVICE_NAME   "WiCAN"
#define WICAN_BLE_PIN       "123456"  // Default PIN — ggf. anpassen

NimBLEClient* pClient = nullptr;
NimBLERemoteCharacteristic* pRxChar = nullptr;
NimBLEAddress wicanAddress;
bool doConnect = false;

// --- Callback: eingehende Notify-Daten vom WiCAN ---
void notifyCallback(NimBLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
    String response = String((char*)pData).substring(0, length);
    response.trim();
    Serial.print("[WiCAN] ");
    Serial.println(response);
    // TODO: Response parsen und verarbeiten
}

// --- Hilfsfunktion: AT-Command / PID senden ---
void sendCommand(const String& cmd) {
    if (pRxChar && pClient && pClient->isConnected()) {
        String withCR = cmd + "\r";
        pRxChar->writeValue((uint8_t*)withCR.c_str(), withCR.length(), false);
        Serial.print("[TX] ");
        Serial.println(cmd);
    } else {
        Serial.println("[WARN] Nicht verbunden, Command verworfen.");
    }
}

// --- BLE Scan Callback: WiCAN-Gerät suchen ---
class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* device) override {
        if (device->getName() == WICAN_DEVICE_NAME) {
            Serial.println("[SCAN] WiCAN Pro gefunden!");
            NimBLEDevice::getScan()->stop();
            wicanAddress = device->getAddress();
            doConnect = true;
        }
    }
};

// --- Verbindung aufbauen und Characteristics holen ---
bool connectToWiCAN() {
    pClient = NimBLEDevice::createClient();

    if (!pClient->connect(wicanAddress)) {
        Serial.println("[BLE] Verbindung fehlgeschlagen.");
        return false;
    }
    Serial.println("[BLE] Verbunden.");

    NimBLERemoteService* pSvc = pClient->getService(WICAN_SERVICE_UUID);
    if (!pSvc) {
        Serial.println("[BLE] NUS Service nicht gefunden.");
        pClient->disconnect();
        return false;
    }

    pRxChar = pSvc->getCharacteristic(WICAN_CHAR_RX_UUID);
    NimBLERemoteCharacteristic* pTxChar = pSvc->getCharacteristic(WICAN_CHAR_TX_UUID);

    if (!pRxChar || !pTxChar) {
        Serial.println("[BLE] Characteristics nicht gefunden.");
        pClient->disconnect();
        return false;
    }

    // Notifications auf TX aktivieren
    if (!pTxChar->subscribe(true, notifyCallback)) {
        Serial.println("[BLE] Notify-Subscription fehlgeschlagen.");
        return false;
    }

    Serial.println("[BLE] Ready.");
    return true;
}

void setup() {
    Serial.begin(115200);
    NimBLEDevice::init("e-up-proxy");

    // Scan starten
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(new ScanCallbacks());
    pScan->setActiveScan(true);
    pScan->start(10, false);  // 10 Sekunden scannen
}

void loop() {
    if (doConnect) {
        doConnect = false;
        if (connectToWiCAN()) {
            // Initialisierungssequenz
            delay(200);
            sendCommand("ATZ");
            delay(500);
            sendCommand("ATE0");
            delay(200);
            sendCommand("ATH1");
            delay(200);
            sendCommand("ATSP0");
            delay(500);

            // Beispiel: SOC-ähnliche Abfrage (PID je nach e-up! anpassen)
            sendCommand("0105");
        }
    }
    delay(100);
}
```

---

## Wichtige Hinweise

### BLE-Passwort (PIN)
Der WiCAN Pro hat standardmäßig ein BLE-Passwort (`123456`, konfigurierbar). Bei Bedarf muss in NimBLE ein `NimBLESecurityCallbacks`-Handler implementiert werden, der den PIN zurückgibt.

### Gleichzeitiger BLE + WiFi Betrieb
Laut offizieller Dokumentation belastet der gleichzeitige Betrieb von BLE und WiFi den ESP32-S3 im WiCAN spürbar. **Empfehlung:** WiFi auf dem Dongle deaktivieren, wenn nur BLE genutzt wird.

### Config-AP wird deaktiviert
Sobald ein BLE-Client verbunden ist, deaktiviert der WiCAN seinen Konfigurations-Access-Point. Konfigurationsänderungen daher **vor** dem BLE-Connect erledigen, oder BLE temporär trennen.

### BLE Performance
Aus den Firmware-Docs: *"It is highly recommended to turn OFF the BLE if not used. Otherwise it might affect the performance."*

### e-up! spezifische PIDs
Standard OBD-II PIDs liefern nur Basisdaten. Batterie-SOC, Ladestand etc. sind **herstellerspezifische PIDs** (UDS/KWP auf CAN). Diese müssen für den e-up! gesondert recherchiert oder durch CAN-Scanning ermittelt werden — das ist unabhängig vom BLE-Layer.

---

## Referenzen

- [WiCAN Firmware GitHub](https://github.com/meatpiHQ/wican-fw)
- [WiCAN Dokumentation](https://meatpihq.github.io/wican-fw/)
- [WiCAN Pro Produktseite](https://www.meatpi.com/products/wican-pro)
- [Nordic UART Service Spec](https://infocenter.nordicsemi.com/topic/com.nordic.infocenter.sdk5.v14.0.0/ble_sdk_app_nus_eval.html)
