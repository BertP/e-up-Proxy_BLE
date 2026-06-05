# Walkthrough: Umstellung auf ESP32-C6

Die "Designentscheidung: Umstellung auf ESP32-C6 als alleinige Zielhardware" wurde erfolgreich im Code abgebildet. Im Folgenden die durchgeführten Änderungen.

## Durchgeführte Änderungen

### 1. Bereinigung der PlatformIO-Konfiguration
Wir haben die `platformio.ini` komplett aufgeräumt und alte, nicht mehr benötigte ESP32-WROOM Umgebungen entfernt.

* **Standard-Umgebung:** Die Standardumgebung wurde auf `esp32c6` gesetzt.
* **OTA-Umgebung (`env:esp32c6`):**
  * Protokoll: `espota` (Port: `192.168.1.55`)
  * Auth: `--auth=eup-proxy-ota`
  * Board: `esp32-c6-devkitc-1`
* **USB-Umgebung (`env:esp32c6-usb`):**
  * Protokoll: `esptool` über `/dev/ttyACM0`
  * Dient als direkter Fallback, falls OTA wie beim letzten Mal fehlschlägt.
* **Native-Tests (`env:native`):** Beibehalten, um die Unit-Tests weiterhin auf dem Host ausführen zu können.

### 2. Code-Anpassungen (`src/main.cpp`)
Da der ESP32-C6 jetzt die einzige Hardware ist, wurden die hardwarespezifischen Initialisierungen (für den Seeed Studio XIAO ESP32-C6) fest integriert:
* Die `#if defined(IS_ESP32_C6)` Weichen wurden entfernt.
* Die Aktivierung des RF-Switches (Pin 3) und der Keramik-Antenne (Pin 14) erfolgt nun bedingungslos bei jedem Boot, was den Code sauberer und weniger fehleranfällig macht.

### 3. Agenten-Anweisungen (`MISSIONPROMPT.md`)
* Um die ständigen Hänger und das "Rumgeeiere" bei zukünftigen Builds zu beheben, wurde die Agenten-Anweisung in der `MISSIONPROMPT.md` aktualisiert. Der fehlerhafte `wsl -d Ubuntu-22.04` Präfix wurde entfernt, da der Agent selbst bereits in Linux läuft. Der korrekte Aufruf lautet nun schlicht: `bash -l -c "pio run"`.

## Ausstehende Schritte
> [!WARNING]
> Aufgrund der aktuellen Server-Hänger konnte der finale Build (`pio run`) noch nicht erfolgreich durchlaufen. 

* **Kompilierung & Cleanup:** Als Nächstes werden wir den PlatformIO-Cache bereinigen, um den hängenden Build-Prozess zu reparieren, und die Firmware kompilieren.
* **Flashen:** Sobald der Build durchläuft, flashen wir das Ganze direkt via USB (`/dev/ttyACM0`).
