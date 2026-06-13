# SPEC vs. Implementation Verification (2026-06-13)

This report highlights all discrepancies between the requirements outlined in `SPEC.md` and the actual, current software state of the `e-up!Proxy` codebase.

> [!NOTE]
> The majority of the system accurately reflects the SPEC (including Data Buffering, UDS queries, JSON payload structure, State Machine transitions, and LED signaling). 

The following deviations were identified:

### 1. [SPEC-01] Home Wi-Fi Configuration
- **SPEC says:** Configured SSIDs / Passwords (e.g., `partlycloudy`).
- **Actual Code:** The code is explicitly hardcoded to use `partlycloudy-iot` for *both* the primary and fallback Wi-Fi networks in `include/config.h` (based on the latest user configuration).

### 2. [SPEC-02] Over-the-Air (OTA) Updates
- **SPEC says:** OTA is active when connected to Home Wi-Fi via `ArduinoOTA` on port 3232, protected by password `eup-proxy-ota`. It explicitly defines OTA Lock Prevention rules (no LittleFS writes, WDT feeding during OTA, block network scans).
- **Actual Code:** **Completely Missing.** There is absolutely no `ArduinoOTA` code anywhere in the project (no `#include <ArduinoOTA.h>`, no `ArduinoOTA.begin()`, and no `ArduinoOTA.handle()`). The feature is defined in the specification but has not been implemented.

### 3. [SPEC-04] Tester Present Keep-Alive
- **SPEC says:** UDS Session is kept alive with `3E 80` (Tester Present, suppress response) sent every 2.5 seconds.
- **Actual Code:** The code sends `3E` instead of `3E 80` (in `src/OBDManager.cpp`). 
  - *Context:* This is an intentional deviation implemented to prevent the ELM327 chip from blocking/timing out. By omitting the `80`, the ECU sends a positive response which instantly unlocks the ELM327 buffer, preventing subsequent OBD queries from failing.

### 4. [SPEC-02] Diagnostics Log Size Limit 
- **SPEC says:** Log file size cap is 50 KB maximum.
- **Actual Code:** The implementation in `src/logger.cpp` uses a `MAX_LOG_SIZE` of 25 KB. It rotates to a backup file (`debug.bak.log`), meaning the *combined* size of current and backup logs effectively reaches 50 KB. This technically satisfies the requirement but does so via a 2-file rotation strategy rather than a single 50 KB file.
