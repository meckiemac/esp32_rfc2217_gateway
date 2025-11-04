# ESP32 RFC2217 Gateway – MCU Runtime Overview

This example turns the `ser2net` codebase into a reusable MCU-oriented
component that can expose multiple UARTs on an ESP32 (or similar target)
via TCP/RFC2217.  The original desktop version relied on `select()` and
gensio; those pieces were reimplemented using FreeRTOS tasks and the
ESP-IDF networking / UART drivers.

## Configuration Flow

The firmware embeds the JSON configuration (`src/config.json`) and parses
it at boot using `cJSON`.  On first boot this embedded file is used to seed
the configuration, after which every runtime change is persisted into NVS so
the device can restart without losing dynamically added ports.  The relevant
sections are:

```json
{
  "serial": [
    {
      "uart": 2,
      "tx_pin": 17,
      "rx_pin": 16,
      "tcp_port": 4000
    },
    {
      "uart": 1,
      "tx_pin": 25,
      "rx_pin": 26,
      "tcp_port": 4001
    }
  ],
  "control": {
    "tcp_port": 4020
  }
}
```

If you want to provision every UART dynamically, the `serial` array can now be
empty (or omitted entirely).  Boot with only the `control` block and add ports
over the control session using `setportconfig` as described below.

Each entry in `serial` defines one UART:

- `uart` – ESP-IDF UART peripheral (`0`, `1`, or `2`).
- `tx_pin`, `rx_pin`, `rts_pin`, `cts_pin` – GPIO assignments.  RTS/CTS
  default to `UART_PIN_NO_CHANGE` when omitted.
- `baud`, `data_bits`, `parity`, `stop_bits`, `flow_control` – initial
  UART parameters (defaults chosen when omitted).
- `tcp_port`, `tcp_backlog` – listener settings; every UART receives its
  own TCP port.

Port identifiers are assigned automatically in load order and are only used
internally by the runtime and control port.

The optional `control` section enables a tiny text interface that lives
on a dedicated TCP socket.  It currently provides `showport`, `help`, and
`quit`.

## Runtime Architecture

The MCU runtime replaces the legacy `select()` loop with three building
blocks:

1. **`ser2net_runtime_start()`** (file `lib/ser2net_mcu/src/runtime.c`)
   - Spins up one TCP listener per configured UART.
   - Allocates a FreeRTOS queue and `max_sessions` worker tasks (each
     executing `session_task()`).
   - Starts an accept loop (`listener_task`) that round-robins across
     the TCP listeners.
   - Optionally starts the control-port task (`ser2net_control_start`).

2. **`session_task()`** (same file) – keeps pulling jobs off the queue.
   For every accepted client it:
   - Opens the corresponding UART (`serial_if->open_serial`).
   - Initialises the RFC2217 state machine (`session_ops->initialise`).
   - Streams data until either side disconnects.

3. **Session layer (`lib/ser2net_mcu/src/session_ops.c`)**
   - Adapts the original RFC2217 implementation to the new runtime.
  - Tracks active sessions per port (using an internal `port_id`, still exposed
    in debug logs and the control port output).
   - Handles Telnet negotiation (`BINARY` + `COM-PORT` options) and all
     RFC2217 commands that make sense on bare-metal (baud rate, data
     bits, parity, stop bits, flow control, purge, and simple
     control-line state).

## Control Port

The control server (`lib/ser2net_mcu/src/control_port.c`) is a straight
TCP loop that accepts a connection, prints a short greeting, and lets
the user issue text commands.  Useful for quick status checks on a
headless device:

```
$ telnet <esp-ip> 4020
ser2net> showport
Port 0 (TCP 4000):
  UART=2 backlog=4 active_sessions=0 mode=telnet enabled=true
  Pins: UART2 TX17 RX16 RTSNA CTSNA
  Defaults: baud=115200 data_bits=8 parity=none stop_bits=1 flow=disabled
Port 1 (TCP 4001):
  UART=1 backlog=4 active_sessions=0 mode=telnet enabled=true
  Pins: UART1 TX25 RX26 RTSNA CTSNA
  Defaults: baud=115200 data_bits=8 parity=none stop_bits=1 flow=disabled
ser2net> quit
```

The task runs independently of the RFC2217 sessions, so it can be left
enabled in production without impacting serial throughput. Die
Monitor-Funktion (`monitor tcp/term`) lässt sich separat über
`ENABLE_MONITORING` kompilieren.

### Dynamic port provisioning

The `setportconfig` command now also creates listeners and UART bindings when a
TCP port is referenced that does not yet exist.  Supply the desired TCP port
followed by the uppercase pin tokens:

```
ser2net> setportconfig 4000 UART2 TX17 RX16 RTSNA CTSNA 115200 8DATABITS NONE 1STOPBIT
Port created.
ser2net> showport 4000
Port 0 (TCP 4000):
  UART=2 backlog=4 active_sessions=0 mode=telnet enabled=true
  Pins: UART2 TX17 RX16 RTSNA CTSNA
  Defaults: baud=115200 data_bits=8 parity=none stop_bits=1 flow=disabled
```

Subsequent `setportconfig` calls update the port in place; you can still adjust
baud rate, framing, flow control, or pins for existing listeners, and the
changes propagate to active sessions when requested.  For a fully dynamic setup
boot with an empty `serial` array, connect to the control port, invoke
`setportconfig` once per UART, then run your RFC2217 clients as usual.

Use `-TX` / `-RX` tokens when a direction should remain unconnected (e.g. pure
monitoring or rawlp). The same shortcut works for modem signals (`-RTS`,
`-CTS`).

### Statische Builds

Schaltet man `ENABLE_DYNAMIC_SESSIONS=0`, lädt das Projekt keine JSON-Konfiguration
mehr. Stattdessen stammen die Ports aus `static_default_ports[]` in `src/main.c`
(oder aus einer eigenen Anwendung). Die Web-API liefert weiterhin Statusdaten,
antwortet bei Schreibzugriffen jedoch mit `403 Forbidden`. Gleiches gilt für den
Control-Port: Kommandos wie `setportconfig` oder `setportenable` werden als
"read-only" quittiert, während `showport` und `disconnect` verfügbar bleiben.

## HTTP API

An embedded HTTP server (built on `esp_http_server`) is available once the
device joins the network.  It mirrors the dynamic runtime features offered by
the control port and keeps the non-volatile store in sync automatically.

- `GET /api/health` – simple status endpoint.
- `GET /api/ports` – list current UART/TCP bindings together with live session
  counts and the assigned GPIO pins.
- `GET /api/system` – aggregate runtime metrics (heap usage, uptime, active
  session count) for dashboard views.
- `POST /api/ports` – create a new listener/UART mapping.  Accepts the same
  fields as the `serial` JSON array (`uart`, `tx_pin`, `rx_pin`, optional
  `rts_pin`/`cts_pin`, plus baud/mode parameters).
- `POST /api/ports/<tcp>` or `/api/ports/<tcp>/config` – update baud rate,
  framing, flow control, idle timeout, or pin assignments.  The payload matches
  the RFC2217 concepts (`baud`, `data_bits`, `parity`, `stop_bits`,
  `flow_control`, `idle_timeout_ms`, `apply_active`, and optional `tx_pin`,
  `rx_pin`, `rts_pin`, `cts_pin`, or `uart`).
- `POST /api/ports/<tcp>/mode` – toggle `raw`, `rawlp`, or `telnet` mode and
  enable/disable the listener.
- `POST /api/ports/<tcp>/disconnect` – drop the currently active TCP session (if
  any) without touching the listener configuration.
- `DELETE /api/ports/<tcp>` – unregister the listener/UART pair entirely.  Any
  active clients are disconnected first; the change is persisted immediately.
- `GET /api/wifi` – report STA/SoftAP status (connected SSID, IP, AP window).
- `POST /api/wifi` – push new Wi-Fi credentials or toggle the provisioning
  SoftAP (`{"ssid":"…", "password":"…", "softap_enabled":true/false}`).
- `DELETE /api/wifi` – forget stored credentials and fall back to provisioning
  SoftAP-only mode.

Every successful HTTP mutation triggers a configuration snapshot, so the next
boot will pick up the updated UART list directly from NVS without requiring a
reflash.

You can exercise the read-only parts of the API quickly via the host-side test
suite:

```bash
SER2NET_ESP_IP=<device-ip> pytest tests/host/test_http_api.py
```

## Logging

- The runtime prints once per listener: `Listener ready: tcp=X ->
  serial port_id=Y`.
- RFC2217 negotiation (`session_ops.c`) uses the `ser2net_session` tag.
- Control-port activity is logged via `ser2net_control`.

These messages make it easy to confirm the mapping on boot:

```
I (...) ser2net_runtime: Listener ready: tcp=4000 -> serial port_id=0
I (...) ser2net_runtime: Listener ready: tcp=4001 -> serial port_id=1
I (...) ser2net_runtime: Control port listening on tcp=4020
```

Feel free to extend the control shell or the JSON schema if you need
additional UART settings (e.g., per-port buffer sizes or post-connect
banners).  The current structure should be a solid foundation for
running `ser2net` as a reusable library inside ESP-IDF applications.
