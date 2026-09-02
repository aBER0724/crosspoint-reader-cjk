#!/usr/bin/env python3
"""Regression checks for WebServer behavior under fragmented X4 heap."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/network/CrossPointWebServer.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
ACTIVITY = (ROOT / "src/activities/network/CrossPointWebServerActivity.cpp").read_text(encoding="utf-8")

assert "buffer.resize(UPLOAD_BUFFER_SIZE)" not in HEADER, "HTTP upload buffer must not throw during construction"
assert "buffer.resize(BUFFER_SIZE)" not in HEADER, "font upload buffer must not throw during construction"
assert "allocateUploadBuffers" not in HEADER + SOURCE + ACTIVITY, "upload buffers must not remain reserved while browsing"
assert "allocateHttpUploadBuffer(state)" in SOURCE, "HTTP buffer must be allocated at upload start"
assert "allocateFontUploadBuffer()" in SOURCE, "font buffer must be allocated at upload start"
assert "state.buffer.reset()" in SOURCE, "HTTP buffer must be released after upload"
assert "fontUpload.buffer.reset()" in SOURCE, "font buffer must be released after upload"
assert "releaseGlyphCaches()" in ACTIVITY, "network startup must reclaim glyph caches"
assert "requireAdminAuth" not in HEADER + SOURCE, "web management token gate must remain removed"
assert "X-CrossPoint-Token" not in SOURCE, "web requests must not require a management token"
assert 'server->setContentLength(responseLength)' not in SOURCE, "settings API must not require a large contiguous heap block"
assert 'sendChunk("]", 1)' in SOURCE, "settings API must finish its streamed JSON array"
assert "_currentClientWrite(" not in SOURCE, "WDT-safe writes must not override the framework path used by HTML responses"
assert "beginWdtSafeResponse" in SOURCE, "settings streaming must keep its bounded watchdog-safe path"
assert 'void CrossPointWebServer::handleGetOpdsServers() const {' in SOURCE
opds_start = SOURCE.index('void CrossPointWebServer::handleGetOpdsServers() const {')
opds_end = SOURCE.index('void CrossPointWebServer::handlePostOpdsServer()', opds_start)
opds_handler = SOURCE[opds_start:opds_end]
assert 'beginWdtSafeResponse("application/json", response.size())' in opds_handler
assert "server->sendContent" not in opds_handler, "OPDS slow-client writes must not use blocking framework writes"
assert "errno == ENOMEM || errno == ENOBUFS" in SOURCE, "transient lwIP memory pressure must be retried"
assert 'server->send_P(200, "text/html", data, len)' in SOURCE, "HTML responses must use the framework response path"
for page in ("HomePage", "FilesPage", "FontsPage", "SettingsPage"):
    assert f"{page}HtmlCompressedSize" in SOURCE, f"{page} must advertise the generated compressed length"

handle_client_start = SOURCE.index("void CrossPointWebServer::handleClient()")
handle_client_end = SOURCE.index("CrossPointWebServer::WsUploadStatus", handle_client_start)
handle_client = SOURCE[handle_client_start:handle_client_end]
assert "server->handleClient()" in handle_client
assert "esp_task_wdt_delete(nullptr)" not in handle_client, "request dispatch must stay watchdog-subscribed"

print("WebServer low-memory/watchdog regression checks passed")
