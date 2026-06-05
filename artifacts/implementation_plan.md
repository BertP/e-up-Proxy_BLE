# Implementation Plan: BLE Architecture Migration (3.0.0-BLE)

## Goal Description
The project specification (`SPEC.md`) was recently updated to version 3.0.0-BLE, dictating a major architecture shift: the proxy must now communicate with the WiCAN OBD2 dongle via Bluetooth Low Energy (BLE GATT) instead of the previous TCP/WiFi socket. 

Currently, the codebase (`OBDManager.cpp` and `NetworkManager.cpp`) still relies on `WiFiClient` and port 35000. This plan outlines the necessary refactoring to align the codebase with `SPEC-01` and `SPEC-04`.

## User Review Required
> [!IMPORTANT]
> This is a major rewrite of the core connectivity and OBD managers. Please confirm if we should proceed with this migration now.

## Open Questions
> [!WARNING]
> 1. In `SPEC-01`, it says the BLE device name prefix is `WiC_`. Should we dynamically discover the rest of the MAC/Name or just connect to the first device matching this prefix?
> 2. Do we have the precise UUIDs for the RX and TX characteristics of the `FFF0` service? (Usually `FFF1` and `FFF2`, but we need to be sure).

## Proposed Changes

### `include/config.h`
- Add BLE configuration constants (Service UUID `FFF0`, device name prefix `WiC_`).
- Adjust state machine state definitions if necessary to reflect `OPERATIONAL_BUFFERING` (BLE only) and `OPERATIONAL_ONLINE` (Wi-Fi + BLE).

---

### `src/NetworkManager.cpp` & `include/NetworkManager.h`
- Modify the state machine to support concurrent BLE and Wi-Fi connections as per `SPEC-01`.
- Implement non-blocking BLE scanning alongside Wi-Fi connecting.
- Manage the 10-minute Wi-Fi shutdown timer and trigger Wi-Fi wakeups when LittleFS buffer has data.

#### [MODIFY] NetworkManager.cpp
#### [MODIFY] NetworkManager.h

---

### `src/OBDManager.cpp` & `include/OBDManager.h`
- Remove `WiFiClient obdClient`.
- Implement `BLEClient` and connect to the `FFF0` service.
- Subscribe to the TX characteristic for notifications.
- Replace the blocking `sendCommand` TCP read loop with an asynchronous/notification-based BLE read mechanism.
- Ensure the ELM327 initialization and UDS Session Keep-Alive (`3E 80`) are correctly routed over BLE.

#### [MODIFY] OBDManager.cpp
#### [MODIFY] OBDManager.h

---

### `src/main.cpp`
- Remove the temporary BLE scanner testing code.
- Ensure `BLEDevice::init()` is called appropriately before NetworkManager starts.

#### [MODIFY] main.cpp

---

### Documentation
- Update `README.md` to reflect the new BLE architecture, removing mentions of TCP Port 35000.
- Update `CHANGELOG.md` with the new version.

#### [MODIFY] README.md
#### [MODIFY] CHANGELOG.md

## Verification Plan

### Automated Tests
- Run `bash -l -c "pio run"` to ensure the project compiles cleanly after substituting TCP for BLE.
- Update native mocks (`pio test -e native`) if they depend on `WiFiClient`.

### Manual Verification
- Ask the user to compile and flash via USB or OTA.
- Verify through serial monitor that BLE device is found and connected.
