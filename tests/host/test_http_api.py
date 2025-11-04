"""Basic smoke tests for the ESP32 HTTP management API.

These tests are intentionally conservative: they only perform read-only
operations so they can be executed repeatedly against a live device without
changing its configuration.  The target device must already run the firmware
and be reachable on the network.

Usage:
    SER2NET_ESP_IP=192.168.x.y pytest tests/host/test_http_api.py
"""

from __future__ import annotations

import os
from typing import Any
import json
from urllib.error import URLError, HTTPError
from urllib.request import urlopen

import pytest


def _base_url() -> str:
    ip = os.environ.get("SER2NET_ESP_IP")
    if not ip:
        pytest.skip("SER2NET_ESP_IP not set – skipping HTTP API tests")
    return f"http://{ip.strip()}"


def _get_json(path: str) -> Any:
    url = f"{_base_url()}{path}"
    try:
        with urlopen(url, timeout=5) as response:
            payload = response.read().decode("utf-8")
    except HTTPError as err:
        pytest.fail(f"HTTP error {err.code} for {url}: {err.reason}")
    except URLError as err:
        pytest.fail(f"Failed to reach {url}: {err.reason}")

    return json.loads(payload)


def test_health_endpoint() -> None:
    payload = _get_json("/api/health")
    assert isinstance(payload, dict)
    assert payload.get("status") == "ok"


def test_ports_collection_schema() -> None:
    payload = _get_json("/api/ports")
    assert isinstance(payload, list)

    if not payload:
        pytest.skip("device exposes no ports yet – nothing else to validate")

    required_keys = {
        "tcp_port",
        "uart",
        "tx_pin",
        "rx_pin",
        "mode",
        "enabled",
        "baud",
        "data_bits",
        "parity",
        "stop_bits",
        "flow_control",
        "idle_timeout_ms",
        "active_sessions",
    }

    for port in payload:
        assert isinstance(port, dict)
        missing = required_keys - port.keys()
        assert not missing, f"missing keys {missing} in port payload {port}"

        assert isinstance(port["tcp_port"], int) and port["tcp_port"] > 0
        assert port["mode"] in {"telnet", "raw", "rawlp"}


def test_system_endpoint() -> None:
    payload = _get_json("/api/system")
    assert isinstance(payload, dict)
    for key in ("free_heap", "min_free_heap", "configured_ports", "active_sessions", "uptime_ms"):
        assert key in payload
        assert isinstance(payload[key], (int, float))


def test_wifi_status_endpoint() -> None:
    payload = _get_json("/api/wifi")
    assert isinstance(payload, dict)
    expected_keys = {
        "sta_configured",
        "sta_connected",
        "sta_ssid",
        "sta_ip",
        "softap_active",
        "softap_force_disabled",
        "softap_remaining_seconds",
    }
    for key in expected_keys:
        assert key in payload
