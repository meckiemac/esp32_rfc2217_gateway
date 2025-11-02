import os
import time
import socket

import pytest
import serial

ESP_IP = os.getenv("SER2NET_ESP_IP")
ESP_PORT = int(os.getenv("SER2NET_ESP_PORT", "4000"))
RAW_PORT = os.getenv("SER2NET_RAW_PORT")
RAWLP_PORT = os.getenv("SER2NET_RAWLP_PORT")

@pytest.mark.skipif(not ESP_IP, reason="SER2NET_ESP_IP not set")
def test_echo_loopback():
    url = f"rfc2217://{ESP_IP}:{ESP_PORT}?logging=debug"
    ser = None
    try:
        try:
            ser = serial.serial_for_url(url, baudrate=115200, timeout=1)
        except serial.SerialException as exc:
            msg = str(exc)
            if ("Remote does not accept parameter change" in msg or
                    "does not seem to support RFC2217" in msg):
                pytest.skip("RFC2217 disabled on target port (raw/rawlp mode).")
            raise
        payload = b"ser2net-test" + bytes([ord('\n')])
        ser.reset_input_buffer()
        ser.write(payload)
        ser.flush()
        time.sleep(0.2)
        data = ser.read(len(payload))
        assert data == payload
        ser.baudrate = 9600
        time.sleep(0.2)
        ser.write(payload)
        ser.flush()
        time.sleep(0.2)
        ser.timeout = 5
        data2 = ser.read(len(payload))
        assert data2 == payload
    finally:
        if ser:
            ser.close()


@pytest.mark.skipif(not (ESP_IP and RAW_PORT), reason="SER2NET_RAW_PORT not set")
def test_raw_loopback():
    payload = b"raw-loopback"
    with socket.create_connection((ESP_IP, int(RAW_PORT)), timeout=5) as sock:
        sock.settimeout(1)
        sock.sendall(payload)
        data = sock.recv(len(payload))
        assert data == payload


@pytest.mark.skipif(not (ESP_IP and RAWLP_PORT), reason="SER2NET_RAWLP_PORT not set")
def test_rawlp_no_loopback():
    payload = b"rawlp-loopback"
    with socket.create_connection((ESP_IP, int(RAWLP_PORT)), timeout=5) as sock:
        sock.settimeout(0.5)
        sock.sendall(payload)
        with pytest.raises(socket.timeout):
            sock.recv(len(payload))
