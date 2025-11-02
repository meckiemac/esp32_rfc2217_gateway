# ESP32 RFC2217 Gateway Example

This example demonstrates how to run the new ser2net FreeRTOS runtime on an ESP32
(ESP-IDF framework via PlatformIO). It joins Wi-Fi, listens for RFC2217 clients,
and forwards data to UART1 (GPIO 17 TX -> GPIO 16 RX by default).

## Wiring

- ESP32 GPIO17 (TX1) -> target RX
- ESP32 GPIO16 (RX1) -> target TX
- Ground shared between boards.
- Optionally loop TX1 to RX1 for a self-test.

## Build & Flash

1. Copy `src/wifi_config.h.example` to `src/wifi_config.h` and fill in your SSID/PSK.
2. Adjust `src/config.json` if you use different pins or baud rates.
3. From this directory:

```bash
pio run -e esp32idf -t upload
pio device monitor
```

On boot you should see Wi-Fi connection logs and the advertised RFC2217 port (default 4000).

## Testing

On your host machine install `pyserial` and run:

```bash
python -m serial.tools.miniterm rfc2217://<esp-ip>:4000 115200
```

Typing in the miniterm window should appear on the device connected to UART1. If you loop TX↔RX you
should see your own keystrokes echoed. You can change line parameters (e.g., `s.baudrate = 9600` in
Python) and observe the ESP32 reconfigure the UART.

## Notes

- The JSON loader currently lives in RAM as an embedded C string. For production you may want to load it
  from SPIFFS/FFat or provide it via OTA.
- Only 3-wire UART (TX/RX/GND) is assumed; no modem control signals are implemented yet.
- Ensure the UART pins you choose are free (GPIO16/17 are safe on most dev boards).
