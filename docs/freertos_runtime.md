# FreeRTOS-basierte Runtime für ser2net

Dieser Entwurf ersetzt die alte `selector.c`-Loop durch Tasks und Queues
unter FreeRTOS. Kernkomponenten:

- `listener_task`: akzeptiert TCP-Clients via
  `ser2net_network_if::accept_client()` und übergibt sie an eine Queue.
- `session_task`: verarbeitet je Verbindung die Telnet/RFC2217-Logik,
  indem in der Schleife `session_ops->process_io()` aufgerufen wird. Die
  eigentliche Protokoll-Implementierung wird außerhalb gekapselt.
- `ser2net_serial_if`: stellt die Verbindung zur UART-Hardware her
  (derzeit ESP32 UART-Treiber via `HardwareSerial`/`uart_driver_*`).
- `ser2net_session_ops`: Adapter zu den bestehenden RFC2217-Funktionen
  aus `telnet.c`/`dataxfer.c`. Hier wird der eigentliche Datenfluss
  zwischen Netzwerk und UART umgesetzt.

Der Code liegt in `freertos/runtime.c` und `freertos/runtime.h`. Die
Dateien sind bewusst unabhängig vom bestehenden Build, damit sie ohne
Auswirkungen auf die klassische Linux-Version entwickelt werden können.

## Nächste Schritte

1. **Netzwerkadapter** implementieren, der `WiFiServer` oder
   `EthernetServer` kapselt und in `ser2net_network_if` einspeist.
2. **Serialadapter** schreiben, der Baudraten/RTS/DTR etc. aus den alten
   `devcfg.c`-Callbacks abbildet.
3. Die bestehenden RFC2217-Funktionen (`telnet.c`, relevante Teile aus
   `dataxfer.c`) so refactoren, dass sie `ser2net_session_ops` bedienen.
4. Timer-/Timeout-Handling in `process_io()` und via
   `xTaskNotify`/`xTimer` konsolidieren.

## Verfügbare Plattformadapter

- `freertos/adapters.c` stellt ab sofort `ser2net_network_if`- und
  `ser2net_serial_if`-Implementierungen für ESP-IDF (`ESP_PLATFORM`) bereit.
  Die Konfiguration erfolgt über kleine Strukturen in `freertos/adapters.h`.
- Nicht unterstützt sind derzeit:
  - IPv6, TLS oder mDNS auf beiden Plattformen.
  - Mehrfach-Nutzer desselben UARTs (die ESP32-Variante installiert die
    Treiber pro Session).
  - Modemstatus-/Linienereignisse, Break-Signal oder RFC2217-seitige
    Signaturmeldungen (GPIO/Modemleitungen werden nicht ausgewertet).
  - Dynamische Baud-/Parity-/Stopbits-Konfiguration nutzt derzeit
    ausschließlich die ESP32 UART-Treiber; Flow-Control-Umschaltung ist
    deaktiviert.
  - Dynamisches Umschalten von Baudrate, Stopbits etc. nach dem Öffnen;
    hierfür müssen spezifische SDK-Aufrufe ergänzt werden.
- `freertos/session_ops.c` enthält einen zustandsbehafteten
  Telnet/RFC2217-Handler, der Baudrate, Daten-/Stoppbits, Parität,
  Flow-Control und Purge-Kommandos verarbeitet und Antworten im Sinne
  von RFC2217 sendet. Fehlende Teile:
  - Modem-/Linienstati, Break-Steuerung, Signature-Update, Masken.
  - Steuerung von DTR/RTS über Hardware (Mangels API im Adapter).
  - Authentifizierung, Banner, Controller-Port, Tracefiles.
  - TLS/mDNS, IPv6, Mehrfach-UART-Teilung, erweiterte Fehlerpfade.
- `freertos/config.h` bietet ein einfaches Application-Interface: Mit
  `ser2net_runtime_config_init()` und `ser2net_session_config_init()`
  lassen sich Defaultwerte setzen; `ser2net_start()` verkettet Adapter,
  Session-Handler und Runtime. Plattform-spezifische Adapter müssen
  weiterhin explizit ausgewählt und konfiguriert werden.
- `freertos/json_config.c` enthält einen JSON-Lader für ESP32. Er
  versteht das Schema unten, füllt die Adapterstrukturen und ruft
  anschließend `ser2net_start()` auf. Weitere Plattformen müssten eigene
  Loader ergänzen. Über `ser2net_json_last_error()` lässt sich nach einem
  Fehlschlag eine Fehlermeldung abfragen.

## JSON-Beispiel (ESP32)

### Build-Flags

Die Datei `ser2net_opts.h` stellt Build-Zeit-Schalter zur Verfügung. Relevante
Kombinationen:

- `ENABLE_DYNAMIC_SESSIONS=0` – Ports werden ausschließlich zur Compile-Zeit
  definiert, JSON-Parsing und REST/Control-Port Schreiboperationen sind
  deaktiviert.
- `ENABLE_CONTROL_PORT=0` – kein Telnet-Shell, Monitoring ist implizit aus.
- `ENABLE_MONITORING=0` – Control-Port bleibt aktiv, Streams (`monitor`) sind
  jedoch abgeschaltet.
- `ENABLE_JSON_CONFIG` wird automatisch deaktiviert, sobald dynamische
  Sessions ausgeschaltet sind.

```json
{
  "sessions": { "max": 2, "stack_words": 4096 },
  "buffers": { "net": 512, "serial": 512 },
  "serial": [
    {
      "uart": 1,
      "tx_pin": 17,
      "rx_pin": 16,
      "baud": 115200,
      "mode": "telnet",
      "idle_timeout_ms": 0
    }
  ]
}
```

**JSON-Limits**
- Keine Validierung via Schema – Parser bricht früh mit `pdFAIL` ab.
- Bei einem Parser-Fehler liefert `ser2net_json_last_error()` eine
  kurze Fehlermeldung.
- Serien-Array wird auf `max_ports` begrenzt; zusätzliche Einträge
  werden ignoriert.
- RFC2217-Features jenseits der Basisoptionen (Masken, Modemstatus usw.)
  müssen weiterhin manuell konfiguriert/implementiert werden.
- `examples/esp32_rfc2217_gateway/` zeigt eine lauffähige PlatformIO-
  Anwendung. Sie nutzt den JSON-Lader, verbindet sich per Wi-Fi und
  startet den RFC2217-Listener auf UART1 (GPIO17/16). README enthält
  Tipps zu Verdrahtung und Tests (z. B. `python -m serial.tools.miniterm
  rfc2217://IP:4000`).
- `tests/host/test_rfc2217_smoke.py` ist ein optionaler Host-Test, der
  via `SER2NET_ESP_IP` auf ein reales Board zugreift. Er prüft Echo und
  Baudratenwechsel über pySerial (übersprungen, wenn keine IP gesetzt).

## Control-Port (MCU-Build)

Wird im JSON eine `control`-Sektion mit `tcp_port` gesetzt, startet ein
kleines Telnet-Shell-Interface parallel zum RFC2217-Listener. Die
aktuellen MCU-Kommandos:

- `help`, `version`, `quit`/`exit`
- `showport [port]` zeigt die vollständige UART/TCP-Konfiguration und
  aktuelle Session-Zahlen (Port kann per TCP-Port oder UART-Nummer gewählt
  werden; die internen `port_id`s dienen nur als Debug-Hinweis).
- `showshortport [port]` liefert dieselben Informationen in einer
  Ein-Zeilen-Darstellung.
- `monitor <tcp|term> <port>` streamt die jeweils eingehenden Daten
  (Richtung wählbar) unverändert/roh auf den Control-Port – identisch
  zum Linux-Original; `monitor stop` beendet die Überwachung.
- `disconnect <port>` löst eine bestehende TCP-Verbindung (falls aktiv).
- `setporttimeout <port> <sek>` passt die Leerlaufzeit an (0 = deaktiviert).
- `setportconfig <port> <baud/flags>` ändert Baudrate, Daten-/Stoppbits,
  Parität, Flow-Control sowie die Pinbelegung. Neben den klassischen Tokens
  (`115200 8DATABITS NONE 1STOPBIT +RTSCTS`) werden Großbuchstaben-Varianten
  wie `UART2 TX17 RX16 RTS18 CTS19` verstanden. Das Präfix `-` deaktiviert eine
  Leitung (`-TX`, `-RX`, `-RTS`, `-CTS`). Existiert der angegebene TCP-Port
  noch nicht, legt der Befehl (bei gesetzten `UARTn` plus mindestens einer
  Richtung) automatisch eine neue Port-Zuordnung an. In Builds ohne
  `ENABLE_DYNAMIC_SESSIONS` antwortet der Befehl mit einer Fehlermeldung.
- `setportcontrol <port> <controls>` setzt DTR/RTS-Leitungen (`RTSHI`, `RTSLO`,
  `DTRHI`, `DTRLO`). Wird bei statischen Builds (`ENABLE_DYNAMIC_SESSIONS == 0`)
  mit einer Fehlermeldung quittiert.
- `setportenable <port> <off|raw|rawlp|telnet>` deaktiviert/aktiviert Ports und
  schaltet den Sitzungsmodus um (OFF beendet aktive Verbindungen). In der
  statischen Konfiguration nur als Lesebefehl verfügbar.
- `idle_timeout_ms` (optional) pro serial-Eintrag definiert die gewünschte
  Leerlaufzeit (0 = deaktiviert); Änderungen per Control-Port werden in die
  Laufzeit-Defaults übernommen.
- Für jede UART kann optional `mode` gesetzt werden (`telnet`, `raw`,
  `rawlp`). `telnet` aktiviert RFC2217/Telnet-Negotiation wie bisher,
  `raw` schaltet alle Telnet-Kommandos ab, `rawlp` verhält sich wie
  `raw`, unterbindet jedoch die Rückrichtung (z. B. für Drucker).

Weitere Linux-spezifische Controller-Befehle (Disconnect, Port-Config,
Line-State-Steuerung) sind bewusst nicht implementiert, weil passende
Hooks auf der MCU-Seite aktuell fehlen.
