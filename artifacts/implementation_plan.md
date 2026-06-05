# [Goal Description]

The user has reconfigured the WiCAN OBD2 dongle to `BLE+Station` mode. This enables a major architectural shift for the `e-up!Proxy`: Instead of the ESP32 dynamically switching its Wi-Fi connection between the car's dongle and the home network, it can now connect to the Home Wi-Fi and the WiCAN dongle via **BLE (Bluetooth Low Energy)** simultaneously.

This plan outlines the required updates to `SPEC.md` before any code changes are made.

> [!IMPORTANT]
> **Major Architectural Change:** Moving to BLE means we can eliminate the complex Wi-Fi state switching. The proxy can maintain a persistent MQTT connection while polling OBD data over BLE.

## User Review Required

Please review the proposed architectural changes to the state machine and communication protocols. 

> [!WARNING]
> BLE communication on the ESP32 can be memory-intensive. We need to ensure that running both the BLE stack and the Wi-Fi/WebServer/MQTT stack concurrently does not exceed the available heap memory.

## Open Questions

> [!IMPORTANT]
> 1. **BLE Protocol Details:** Does the WiCAN dongle emulate a standard serial port over BLE (e.g., using the Nordic UART Service - NUS) or does it have a custom GATT service/characteristic for sending and receiving ELM327 commands?
> 2. **State Machine:** Do we completely remove the local LittleFS data buffering queue, given that the ESP32 can now publish to MQTT in real-time while connected to the car (as long as the car is within home Wi-Fi range)? Or do we keep the buffer for when the car drives away from the home Wi-Fi?

## Proposed Changes

### [SPEC.md]

We will rewrite sections of `SPEC.md` to reflect the new architecture.

#### [MODIFY] [SPEC.md](file:///home/bert/projects/e-up-Proxy_BLE/SPEC.md)

1. **Header & Architecture:** 
   - Update hardware description to indicate BLE usage for the WiCAN Dongle.
   - Increment Target Firmware Version to `3.0.0-BLE` (or similar).

2. **[SPEC-01] System States & Wi-Fi Logic:**
   - **Remove:** The "Dongle First exklusiv" Wi-Fi switching logic.
   - **Add:** New concurrent state logic where the system connects to Home Wi-Fi and BLE concurrently.
   - **New States:**
     - `WIFI_CONNECTING` / `BLE_CONNECTING`
     - `OPERATIONAL_BUFFERING` (Connected to BLE, disconnected from Wi-Fi - car is away from home)
     - `OPERATIONAL_ONLINE` (Connected to both - real-time MQTT streaming)

3. **[SPEC-02] Diagnostics & Logs:**
   - **Update:** Remove `[SWITCH]` logs for Wi-Fi.
   - **Add:** BLE connection/disconnection logs.

4. **[SPEC-03] Data Buffering Contract:**
   - Modify to state that buffering is only active when Home Wi-Fi is unreachable. When Home Wi-Fi is connected, payloads are published immediately without hitting LittleFS to save flash wear.

5. **[SPEC-04] OBD2 Metrics:**
   - **Update:** Replace TCP Socket (Port 35000) instructions with BLE ELM327 characteristic read/write instructions.

## Verification Plan

### Manual Verification
- Review the updated `SPEC.md` for logical consistency and adherence to the new BLE approach.
- Ensure the state machine logic correctly handles scenarios where the car drives out of Home Wi-Fi range while maintaining the BLE connection to the dongle.
