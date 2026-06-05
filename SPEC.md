# SPEC.md — e-up!Proxy Product Specification
> **Target Firmware Version:** `3.0.0-BLE`  
> Last updated: 2026-06-05

This file describes **what** the system does. For agent behaviour, build tooling, and guardrails, see `MISSIONPROMPT.md`.

### How to reference sections

Each section has a stable ID (e.g. `SPEC-01`). Use these IDs when communicating changes to the agent:
> "SPEC-04 has changed — re-read only that section."

The agent reads only the referenced section, not the entire file.

---

## [SPEC-01] System States, Connectivity Logic & LED Signaling
> Last changed: 2026-06-05 · Complete rewrite for BLE Architecture

The proxy, which is permanently installed in the vehicle, must implement a State Machine to handle network connectivity. It connects to the vehicle's OBD2 dongle via **BLE (Bluetooth Low Energy)** and to the Home Wi-Fi via standard 802.11 Wi-Fi. These connections can be active simultaneously. Status must be visually communicated via the onboard Blue LED (GPIO 2) using non-blocking blink patterns.

### Known Networks / Devices

| Role | Count | Details |
|---|---|---|
| Home Wi-Fi | 2 | Configured SSIDs / Passwords (e.g., `partlycloudy`) |
| Dongle BLE | 1 | BLE Device Name Prefix `WiC_` (e.g., `WiC_fc012cdbd501`), Service UUID `FFF0` |

### Connection States & LED Blink Codes

The system operates based on the availability of the two connections (BLE and Wi-Fi):

**State: DISCONNECTED / SCANNING**
- Logic: The proxy is booting or has lost all connections. It continuously scans for Home Wi-Fi and the WiCAN BLE device simultaneously.
- LED Pattern: Fast blinking (100ms ON / 100ms OFF).

**State: OPERATIONAL_BUFFERING (Only BLE Connected)**
- Logic: The vehicle is away from home or driving. The proxy is connected to the WiCAN dongle via BLE. It fetches OBD2 metrics every 150 seconds and buffers the fetched data locally in a FIFO queue on LittleFS.
- LED Pattern: Distinct double-blink (2 quick blinks every 2 seconds).

**State: OPERATIONAL_ONLINE (Wi-Fi Connected, BLE optional)**
- Logic: The vehicle is at home. The proxy connects to Home Wi-Fi, syncs time via NTP, and instantly flushes all buffered payloads to the MQTT Broker. If BLE is also connected (ignition is on), it continues to poll the car and streams the data in real-time to MQTT without buffering it to flash. A minimal background WebServer is active.
- LED Pattern: Solid ON (or very slow breathing pattern).

### Wi-Fi Power-Saving & Lifecycle

To conserve energy in the vehicle, the Home Wi-Fi radio is strictly managed:
1. **Demand-Driven Scanning:** Background Wi-Fi scanning is ONLY active when there are un-flushed payload files buffered in LittleFS.
2. **Auto-Shutdown:** After connecting to Home Wi-Fi and flushing all data to MQTT, the Wi-Fi connection is kept alive for a maximum of **10 minutes** (allowing time for OTA updates, WebServer access, and debugging).
3. **Deep Idle:** After 10 minutes, the Wi-Fi radio is explicitly disabled (`WiFi.mode(WIFI_OFF)`). It will remain off until new telemetry data is buffered by the BLE OBD process.

### Device Resilience & Connection Safeguards

- **Hardware Watchdog (WDT):** A hardware watchdog is configured with a safety timeout margin of 30 seconds (`WDT_TIMEOUT_S 30`) to absorb network negotiation and diagnostic query delays without triggering unwanted resets.
- **Asynchronous Connectivity:** Wi-Fi connections and BLE scanning are established using non-blocking routines to ensure the watchdog is continuously fed.
- **Boot-Loop Protection:** An NVS crash counter is tracked in preference namespace `proxy` (key `bootcrash`). If the device fails to boot stably 5 consecutive times, a boot loop warning is logged. The counter is automatically zeroed once NTP time sync is confirmed on the Home network.
- **NVS Partition Resilience & Auto-Format:** NVS is initialized using the low-level ESP-IDF `nvs_flash_init()` handler to survive partition layout changes.

---

## [SPEC-02] Diagnostics, WebServer & Log Management
> Last changed: 2026-06-05 · Updated switch logs for BLE

### Log Storage

- **Mechanism:** LittleFS, dedicated file `debug.log`.
- **Size cap:** 50 KB maximum. When the limit is reached, oldest entries are purged automatically (circular/ring buffer behaviour).

### Log Entry Format

All entries use a consistent prefix for easy filtering:

```
[hh:mm:ss] [<LEVEL>] <message>
```

- `hh:mm:ss` — local wall-clock time (`Europe/Berlin`), used **after** NTP synchronisation is confirmed. Before NTP sync, fall back to `[Up HH:MM:SS]` (uptime since boot) and prefix the entry with `[NO-NTP]`.
- `<LEVEL>` — one of `BOOT`, `SCAN`, `CONN`, `SWITCH`, `DATA`, `ERROR`.

### Transition Logs (BLE & Wi-Fi)

Logged when the proxy connects or disconnects from networks:

```
[17:42:11] [CONN] Connected to Home Wi-Fi "partlycloudy"
[17:42:12] [CONN] BLE connection established to "WiC_fc012cdbd501"
[17:50:00] [SWITCH] Home Wi-Fi lost. Entering OPERATIONAL_BUFFERING.
```

### Minimal WebServer

- Active **only** when connected to Home Wi-Fi.
- Endpoints:
  - `GET /debug` — Streams the full contents of `debug.log` as `text/plain`.
  - `GET /obd` — Streams OBD connection logs as `text/plain`.
  - `GET /mqtt` — Streams MQTT broker logs as `text/plain`.
  - `GET /status` — Returns a JSON object with system health metrics, Wi-Fi details, BLE connection state, MQTT status, queue size, and the latest OBD query values.
  - `GET /files` — Returns a JSON array listing all currently buffered telemetry payload files in LittleFS.
  - `POST /clear` — Manually clears/wipes the persistent LittleFS telemetry queue directory.

### Over-the-Air (OTA) Updates
- Active **only** when connected to Home Wi-Fi.
- **Protocol:** ArduinoOTA on standard port `3232`.
- **Hostname:** `eup-proxy`
- **Security:** Access is protected by the upload password `eup-proxy-ota`.
- **OTA Lock Prevention Specifications:**
  1. **Zero Filesystem Activity during OTA:** No LittleFS file operations are permitted inside any `ArduinoOTA` callbacks.
  2. **Watchdog Keeping (WDT Feed):** Must feed WDT via `ArduinoOTA.onProgress`.
  3. **Network Scan Lock:** The proxy must block all periodic network scanning and BLE operations while an active OTA update is in progress.

---

## [SPEC-03] Data Buffering Contract
> Last changed: 2026-06-05 · Clarified buffering context (in-car usage)

Because the ESP32 travels in the vehicle, it cannot communicate with the Home MQTT broker while driving. All metrics must be queued locally on the flash memory while away from home and flushed once the vehicle returns to the Home Wi-Fi range.

### Buffer Mechanism

- **Type:** FIFO (First-In, First-Out) queue.
- **Storage:** LittleFS.
- **Buffering Condition:** Only active when Home Wi-Fi is DISCONNECTED and BLE is CONNECTED. If both are connected, payloads are sent straight to MQTT without flash storage to reduce wear.
- **Flush trigger:** Immediately upon connecting to Home Wi-Fi. After a successful flush, the 10-minute Wi-Fi shutdown timer is triggered.

### Payload Structure (JSON)

The buffered payload mirrors the `eup/data` MQTT schema:

```json
{
  "soc": 82.5,
  "volt": 12.4,
  "temp": 18.0,
  "range": 202.5,
  "power": -2100.0,
  "odo": 42150.0,
  "service_days": 180.0,
  "service_km": 7500.0,
  "bat_cap": 61.5,
  "tp_alarm": 0.0,
  "ts": 1779294900,
  "src": "CAR_BUFFERED"
}
```

---

## [SPEC-04] OBD2 Metrics — BLE DID Mapping (WiCAN / STN2120)
> Last changed: 2026-06-05 · Changed communication protocol from TCP to BLE GATT

Source: `OBDManager.h`. The WiCAN Dongle is accessed via **Bluetooth Low Energy (BLE)** using the custom Serial Service UUID `0000FFF0-0000-1000-8000-00805F9B34FB`. ELM327 AT commands and UDS requests are written to the RX characteristic, and responses are read from the TX characteristic via notifications.

A UDS Extended Diagnostic Session (`10 03`) is opened once per connection on ECU `7E5` and kept alive with `3E 80` (Tester Present, suppress response).

### ECU Overview

| ECU | CAN Header | Scope |
|---|---|---|
| BMS (Battery Management) | `7E5` | SoC, temperature, battery capacity |
| Dashboard / Gateway | `7E0` | Odometer, service intervals, tyre pressure |

### Read Group A — Cyclic (every 150 s, during driving and charging)

| MQTT field | Method | ECU | DID | Raw formula / Type | Unit |
|---|---|---|---|---|---|
| `soc` | `querySoC()` | `7E5` | `02 8C` | `raw × 0.4` (1 byte, `uint8_t`) | `%` |
| `temp` | `queryTemp()` | `7E5` | `2A 0B` | `raw × 0.015625` (2 bytes, `int16_t` signed) | `°C` |
| `bat_cap` | `queryBatteryCapacity()` | `7E5` | `22 E1` | `raw × 0.1` (2 bytes, `int16_t`) | `Ah` |
| `volt` | `queryVoltage()` | — | `AT RV` | direct float | `V` |
| `tp_alarm` | `queryTirePressureAlarm()` | `7E0` | `02 1A` | `raw` (1 byte, 0=OK) | — |

### Read Group B — Pre-flight / slow poll (every 10 min)

| MQTT field | Method | ECU | DID | Raw formula | Unit |
|---|---|---|---|---|---|
| `odo` | `queryOdometer()` | `7E0` | `22 03` | `raw` (3 bytes) | `km` |
| `service_days` | `queryDaysToService()` | `7E0` | `02 48` | `raw` (2 bytes) | `d` |
| `service_km` | `queryKmToService()` | `7E0` | `02 47` | `raw` (2 bytes) | `km` |

Group B is also read **once immediately** after WiCAN BLE connection is established, then on the 10-minute timer.

### Session & Protocol Notes

- ELM327 init sequence (once per BLE connect): `AT E0` · `AT L0` · `AT H0` · `AT SP 6` · `AT AT 1` · `AT ST FF`
- CAN protocol: 11-bit, 500 kbps (`AT SP 6`)
- Header switching: `AT SH 7E5` / `AT SH 7E0`
- **Command Timeout:** 2.0 seconds (`OBD_CMD_TIMEOUT_MS`).
- **Tester Present Keep-Alive:** `3E 80` sent every 2.5 seconds (`TESTER_PRESENT_INTERVAL_MS`).

---

## [SPEC-05] MQTT Topics, Broker & Home Assistant Integration
> Last changed: 2026-06-05 · Architecture remains identical

*(No changes to MQTT payload schema, retaining Europe/Berlin timezone offsets and auto-discovery JSONs).*

### Topic: `eup/data`
- **Description:** All cyclically measured vehicle telemetry as a consolidated JSON object.
- **Retained:** `true`

### Topic: `eup/lastSync`
- **Description:** Timestamp of the last successful data transmission. Used to monitor data freshness in Home Assistant.
- **Retained:** `true`
- **Format:** ISO-8601 string with timezone offset (e.g. `2026-06-05T13:42:00+02:00`).

---

## [SPEC-06] Changelog

| Date | Section | Change |
|---|---|---|
| 2026-05-31 | SPEC-01, SPEC-02, SPEC-04 | Dynamic Gateway IP resolution, Uptime-Timestamp prefix, and board voltage (AT RV) fallback |
| 2026-06-05 | All | **Major Architecture Shift:** Transitioned from TCP/Wi-Fi to BLE GATT for OBD dongle communication. Updated state machine to handle concurrent BLE and Home Wi-Fi connections, reflecting in-car proxy operation. Target version `3.0.0-BLE`. |
