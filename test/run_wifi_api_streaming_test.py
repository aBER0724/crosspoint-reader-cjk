#!/usr/bin/env python3
"""Regression checks for low-memory, watchdog-safe Wi-Fi API responses."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")

scan_start = SOURCE.index("void CrossPointWebServer::handleWifiScan() const {")
scan_end = SOURCE.index("void CrossPointWebServer::handleWifiSave() const {", scan_start)
scan = SOURCE[scan_start:scan_end]

wifi_list_start = SOURCE.index("void CrossPointWebServer::handleWifiList() const {")
wifi_list_end = SOURCE.index("void CrossPointWebServer::handleWifiDelete() const {", wifi_list_start)
wifi_list = SOURCE[wifi_list_start:wifi_list_end]

for name, handler in (("scan", scan), ("list", wifi_list)):
    assert "String json" not in handler, f"Wi-Fi {name} must not build the full response on the heap"
    assert 'beginWdtSafeChunkedResponse("application/json")' in handler
    assert 'writeWdtSafeChunk("[", 1)' in handler
    assert 'writeWdtSafeChunk("]", 1)' in handler
    assert "endWdtSafeChunkedResponse()" in handler
    assert "server->sendContent" not in handler, f"Wi-Fi {name} must use watchdog-safe nonblocking writes"

assert "if (written >= sizeof(buf)) continue" in scan
assert scan.index("if (written >= sizeof(buf)) continue") < scan.index('writeWdtSafeChunk(",", 1)')
assert "appendEscapedJsonString" in wifi_list
assert "if (escapedLength == 0 && !credential.ssid.empty()) continue" in wifi_list
assert wifi_list.index("if (escapedLength == 0") < wifi_list.index('writeWdtSafeChunk(",", 1)')
assert "String escaped" not in wifi_list

print("Wi-Fi API low-memory/watchdog regression checks passed")
