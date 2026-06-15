# Implementierungsplan: Fix OBD UDS Kommunikation

Das Symptom "nur die Bordspannung wurde aufgezeichnet" bedeutet, dass die Proxy-Software bei allen "echten" Fahrzeugabfragen (SOC, Temp, etc.) scheitert und auf ein reines Spannungs-Fallback umschaltet. 

## Ursachen-Analyse
Wenn die Bordspannung (AT RV) erfolgreich gelesen wird, die UDS-Abfragen aber fehlschlagen, gibt es drei primäre Fehlerquellen:
1. **CAN-Filter (CRA):** Der WiCAN-Dongle ignoriert möglicherweise die Antworten des Autos, weil der Empfangsfilter (CAN Receive Address) beim Wechseln des Headers (`AT SH`) nicht automatisch angepasst wird.
2. **Tester Present:** Unser vorheriger Fix auf `3E` ist ungültig nach UDS-Standard, es muss `3E 00` (Tester Present mit positiver Antwort) heißen. Ein ungültiges `3E` führt zu einem Fehler im Steuergerät (NRC), wodurch die Diagnose-Sitzung sofort wieder beendet wird.

## Proposed Changes

### `src/OBDManager.cpp`
- **[MODIFY]** `setHeader()`: Wir fügen einen expliziten Befehl `AT CRA` (CAN Receive Address) hinzu. Wenn wir an `7E5` senden, weisen wir den Dongle explizit an, auf `7ED` zu lauschen. Bei `7E0` auf `7E8`.
- **[MODIFY]** `runOBDKeepAlive()`: Wir ändern den Befehl von `3E` auf den sauberen Standardbefehl `3E 00`. So antwortet das Auto positiv, der Dongle blockiert nicht, und das Steuergerät bricht die Diagnose-Sitzung nicht ab.

## Open Questions / User Review Required

> [!IMPORTANT]
> **Bevor ich den Code anpasse, brauche ich idealerweise die Fehlerprotokolle vom Dongle!**
> 
> Da dein Proxy aktuell wieder im WLAN sein sollte, öffne bitte deinen Browser und rufe folgende URL auf:
> `http://<IP-DEINES-PROXYS>/obd`
> 
> Bitte kopiere den Text, der dort angezeigt wird, und füge ihn hier im Chat ein. Daran können wir zu 100% genau sehen, mit welchem Fehlercode das Auto die Abfragen ablehnt oder ob der Dongle "NO DATA" meldet.
>
> Falls du gerade keinen Zugriff auf die Logs hast, kannst du mir auch einfach das Go geben, und ich setze den Fix auf Verdacht um!
