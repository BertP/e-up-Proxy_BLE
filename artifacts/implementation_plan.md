# Implementation Plan — Migration to ESP32-C6 Target Hardware

We are migrating the e-up!Proxy firmware to support the **ESP32-C6** as the sole target hardware. This involves updating the PlatformIO configuration, cleaning up legacy ESP32-WROOM-32 configurations, and ensuring all ESP32-C6 specific initializations (like RF switch/ceramic antenna pins) are active.

## Proposed Changes

### Build Configuration

#### [MODIFY] [platformio.ini](file:///home/bert/projects/e-up!Proxy/platformio.ini)
* Set `default_envs = esp32c6` (or `esp32c6-ota`).
* Update `env:esp32c6` to include the OTA upload configuration:
  * `upload_protocol = espota`
  * `upload_port = 192.168.1.55` (default IP from local network)
  * `upload_flags = --auth=eup-proxy-ota`
* Configure `env:esp32c6-usb` (or rename `env:usb`) as the USB flashing fallback for the ESP32-C6 board:
  * `board = esp32-c6-devkitc-1`
  * `upload_protocol = esptool`
  * `upload_port = /dev/ttyACM0`
  * `upload_speed = 460800`
* Remove or comment out the legacy ESP32-WROOM-32 environments (`env:esp32dev` and old `env:usb`) to prevent accidental compilation/flashing for the wrong target.

### Source Code

#### [MODIFY] [main.cpp](file:///home/bert/projects/e-up!Proxy/src/main.cpp)
* Remove the `#if defined(IS_ESP32_C6)` conditional check around the RF switch/ceramic antenna configuration in `setup()`, making the ESP32-C6 RF switch and ceramic antenna configuration unconditional.
* Alternatively, ensure `IS_ESP32_C6=1` is always defined and clean up any legacy ESP32-WROOM specific conditional blocks if applicable.

---

## Verification Plan

### Automated Tests
* Run compilation for the default env `esp32c6` to ensure it builds successfully:
  ```bash
  wsl -d Ubuntu-22.04 --cd /home/bert/projects/e-up!Proxy bash -l -c "pio run"
  ```
* Run compilation for the usb env `esp32c6-usb` to verify usb configuration:
  ```bash
  wsl -d Ubuntu-22.04 --cd /home/bert/projects/e-up!Proxy bash -l -c "pio run -e esp32c6-usb"
  ```

### Manual Verification
* Flash the built binary to the ESP32-C6 target via USB (`/dev/ttyACM0`) once confirmed by the user.
* Confirm booting and serial output.
