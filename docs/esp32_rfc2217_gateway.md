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
      "port_id": 0,
      "uart": 2,
      "tx_pin": 17,
      "rx_pin": 16,
      "tcp_port": 4000
    },
    {
      "port_id": 1,
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
over the control session using `addport` as described below.

Each entry in `serial` defines one UART:

- `port_id` – opaque identifier used by the runtime, passed to the
  session layer and control port.
- `uart` – ESP-IDF UART peripheral (`0`, `1`, or `2`).
- `tx_pin`, `rx_pin`, `rts_pin`, `cts_pin` – GPIO assignments.  RTS/CTS
  default to `UART_PIN_NO_CHANGE` when omitted.  As an alternative syntax
  you can use a single string such as `"pin_map": "uart2:tx17,rx16[,rts18]"`;
  the control port will show whichever form was last applied.
- `baud`, `data_bits`, `parity`, `stop_bits`, `flow_control` – initial
  UART parameters (defaults chosen when omitted).
- `tcp_port`, `tcp_backlog` – listener settings; every UART receives its
  own TCP port.

The optional `control` section enables a tiny text interface that lives
on a dedicated TCP socket.  It currently provides `status`, `help`, and
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
   - Tracks active sessions per `port_id` (used by the control port).
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
ser2net> status
Active ports:
  port_id=0 sessions=0
  port_id=1 sessions=0
ser2net> quit
```

The task runs independently of the RFC2217 sessions, so it can be left
enabled in production without impacting serial throughput.

### Dynamic port provisioning

With the `addport` command you can create listeners and UART bindings on the
fly.  Example (creates the former `uart2` mapping from the JSON snippet above):

```
ser2net> addport port=0 tcp=4000 uart=2 tx=17 rx=16 mode=raw baud=115200
Port added.
ser2net> showport 4000
Port 0 (TCP 4000):
  UART=2 backlog=4 active_sessions=0 mode=raw enabled=true
  Pins: TX=17 RX=16 RTS=n/a CTS=n/a
  Pin map: uart2:tx17,rx16
  Defaults: baud=115200 data_bits=8 parity=none stop_bits=1 flow=disabled
```

Any field that `setportconfig` understands (baud/data bits/parity/flow) can be
adjusted after the port exists.  For a fully dynamic setup boot with an empty
`serial` array, connect to the control port, invoke `addport` for each UART,
then run your RFC2217 clients as usual.

## HTTP API

An embedded HTTP server (built on `esp_http_server`) is available once the
device joins the network.  It mirrors the dynamic runtime features offered by
the control port and keeps the non-volatile store in sync automatically.

- `GET /api/health` – simple status endpoint.
- `GET /api/ports` – list current UART/TCP bindings together with live session
  counts and the resolved `pin_map` string.
- `POST /api/ports` – create a new listener/UART mapping.  Accepts the same
  fields as the `serial` JSON array (including optional `pin_map`).
- `POST /api/ports/<tcp>` or `/api/ports/<tcp>/config` – update baud rate,
  framing, flow control, idle timeout, or pin assignments.  The payload matches
  the RFC2217 concepts (`baud`, `data_bits`, `parity`, `stop_bits`,
  `flow_control`, `idle_timeout_ms`, `apply_active`, and optional `pin_map`).
- `POST /api/ports/<tcp>/mode` – toggle `raw`, `rawlp`, or `telnet` mode and
  enable/disable the listener.
- `POST /api/ports/<tcp>/disconnect` – drop the currently active TCP session (if
  any) without touching the listener configuration.

Every successful HTTP mutation triggers a configuration snapshot, so the next
boot will pick up the updated UART list directly from NVS without requiring a
reflash.

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
