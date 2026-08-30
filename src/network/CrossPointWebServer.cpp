#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <FontManager.h>
#include <FsHelpers.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <map>

#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "OpdsServerStore.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "StoragePathPolicy.h"
#include "WebDAVHandler.h"
#include "WifiCredentialStore.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FontsPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "util/BookCacheUtils.h"
#include "util/ExternalFontLabel.h"
#include "util/TaskWatchdog.h"

namespace {
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;
constexpr size_t MAX_WEB_UPLOAD_BYTES = 512UL * 1024UL * 1024UL;
constexpr unsigned long MAX_WEB_UPLOAD_MS = 15UL * 60UL * 1000UL;
constexpr size_t MAX_WEB_LIST_ENTRIES = 1000;
constexpr uint64_t WEB_STORAGE_RESERVE_BYTES = 8ULL * 1024ULL * 1024ULL;

bool parseUint32Arg(const String& value, uint32_t& result) {
  if (value.isEmpty()) return false;

  uint32_t parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    if (parsed > (UINT32_MAX - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  result = parsed;
  return true;
}

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

// WebSocket upload state
FsFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedItemName(const char* name) { return StoragePathPolicy::isProtectedItemName(name); }

bool isProtectedItemName(const String& name) { return isProtectedItemName(name.c_str()); }

bool isProtectedWebPath(const String& path) { return StoragePathPolicy::isProtectedPath(normalizeWebPath(path)); }

bool endsWithIgnoreCase(const char* value, const char* suffix) {
  if (value == nullptr || suffix == nullptr) {
    return false;
  }

  const size_t valueLen = strlen(value);
  const size_t suffixLen = strlen(suffix);
  if (suffixLen > valueLen) {
    return false;
  }

  const char* start = value + (valueLen - suffixLen);
  for (size_t i = 0; i < suffixLen; i++) {
    const unsigned char left = static_cast<unsigned char>(start[i]);
    const unsigned char right = static_cast<unsigned char>(suffix[i]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }

  return true;
}

size_t appendEscapedJsonString(char* output, const size_t outputSize, const char* input) {
  if (output == nullptr || outputSize == 0) {
    return 0;
  }

  size_t outPos = 0;
  const char* src = input == nullptr ? "" : input;

  while (*src != '\0') {
    const unsigned char c = static_cast<unsigned char>(*src);
    const char* replacement = nullptr;

    switch (c) {
      case '"':
        replacement = "\\\"";
        break;
      case '\\':
        replacement = "\\\\";
        break;
      case '\b':
        replacement = "\\b";
        break;
      case '\f':
        replacement = "\\f";
        break;
      case '\n':
        replacement = "\\n";
        break;
      case '\r':
        replacement = "\\r";
        break;
      case '\t':
        replacement = "\\t";
        break;
      default:
        break;
    }

    if (replacement != nullptr) {
      const size_t replacementLen = strlen(replacement);
      if (outPos + replacementLen >= outputSize) {
        output[0] = '\0';
        return 0;
      }
      memcpy(output + outPos, replacement, replacementLen);
      outPos += replacementLen;
    } else if (c < 0x20) {
      if (outPos + 6 >= outputSize) {
        output[0] = '\0';
        return 0;
      }
      const int written = snprintf(output + outPos, outputSize - outPos, "\\u%04x", c);
      if (written != 6) {
        output[0] = '\0';
        return 0;
      }
      outPos += 6;
    } else {
      if (outPos + 1 >= outputSize) {
        output[0] = '\0';
        return 0;
      }
      output[outPos++] = static_cast<char>(c);
    }
    src++;
  }

  output[outPos] = '\0';
  return outPos;
}

bool isReaderFontFamilySetting(const SettingInfo& s) { return s.key != nullptr && strcmp(s.key, "fontFamily") == 0; }

bool isUiFontFamilyKey(const char* key) { return key != nullptr && strcmp(key, "uiFontFamily") == 0; }

bool isLanguageSettingKey(const char* key) { return key != nullptr && strcmp(key, "language") == 0; }

String buildChildPath(String parentPath, const String& childName) {
  if (!parentPath.endsWith("/")) {
    parentPath += "/";
  }
  parentPath += childName;
  return parentPath;
}

bool uploadWouldExceedLimit(const size_t currentSize, const size_t incomingSize) {
  return currentSize > MAX_WEB_UPLOAD_BYTES || incomingSize > MAX_WEB_UPLOAD_BYTES - currentSize;
}

bool uploadTimedOut(const unsigned long startedAt) {
  return startedAt != 0 && millis() - startedAt > MAX_WEB_UPLOAD_MS;
}

bool storageHasSpaceForUpload(const size_t pendingBytes) {
  const uint64_t freeBytes = Storage.freeBytes();
  if (freeBytes == 0) {
    return false;
  }

  return freeBytes >= WEB_STORAGE_RESERVE_BYTES + static_cast<uint64_t>(pendingBytes);
}

bool parseUploadSizeToken(const String& token, size_t& outSize, bool& exceedsLimit) {
  outSize = 0;
  exceedsLimit = false;
  bool valid = token.length() > 0;
  int digitStart = (valid && token[0] == '+') ? 1 : 0;
  if (digitStart > 0 && token.length() < 2) {
    valid = false;
  }
  for (int i = digitStart; i < (int)token.length() && valid; i++) {
    if (!isdigit((unsigned char)token[i])) {
      valid = false;
      break;
    }
    const size_t digit = static_cast<size_t>(token[i] - '0');
    if (outSize > (MAX_WEB_UPLOAD_BYTES - digit) / 10) {
      exceedsLimit = true;
      return true;
    }
    outSize = outSize * 10 + digit;
  }
  if (outSize > MAX_WEB_UPLOAD_BYTES) {
    exceedsLimit = true;
  }
  return valid;
}

class WdtSafeWebServer : public WebServer {
 public:
  using WebServer::WebServer;

  void beginWdtSafeResponse(const char* contentType, const size_t contentLength) {
    writeFailed_ = false;
    _contentLength = CONTENT_LENGTH_NOT_SET;
    String header;
    _prepareHeader(header, 200, contentType, contentLength);
    writeWithWatchdog(header.c_str(), header.length());
  }

  bool writeWdtSafeContent(const char* data, size_t len) {
    if (writeFailed_) return false;
    writeWithWatchdog(data, len);
    return !writeFailed_;
  }

  bool writeFailed() const { return writeFailed_; }

 private:
  size_t writeWithWatchdog(const char* data, size_t len) {
    constexpr unsigned long WRITE_TIMEOUT_MS = 30000;
    NetworkClient& currentClient = client();
    const int fd = currentClient.fd();
    if (fd < 0) {
      writeFailed_ = true;
      return 0;
    }

    size_t sent = 0;
    unsigned long lastProgressAt = millis();
    while (sent < len) {
      resetTaskWatchdogIfSubscribed();
      const size_t chunkSize = len - sent;
      const int written = ::send(fd, data + sent, chunkSize, MSG_DONTWAIT);
      if (written > 0) {
        sent += static_cast<size_t>(written);
        lastProgressAt = millis();
        continue;
      }

      const bool retryable = written == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
                             errno == ENOMEM || errno == ENOBUFS;
      if (retryable && millis() - lastProgressAt < WRITE_TIMEOUT_MS) {
        delay(1);
        continue;
      }

      LOG_ERR("WEB", "Socket write failed: fd=%d sent=%zu/%zu errno=%d free=%u max=%u", fd, sent, len, errno,
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      writeFailed_ = true;
      currentClient.stop();
      break;
    }
    return sent;
  }

  bool writeFailed_ = false;
};
}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() = default;

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server.reset(new WdtSafeWebServer(port));

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Add Access-Control-Allow-* headers to every response so web-based clients
  // and PWAs on other origins can use the HTTP API. Preflight OPTIONS requests
  // are answered in handleNotFound().
  server->enableCORS(true);

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { buildSettingsCache(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });

  // WiFi credential management endpoints (CJK)
  server->on("/api/wifi/scan", HTTP_GET, [this] { handleWifiScan(); });
  server->on("/api/wifi/save", HTTP_POST, [this] { handleWifiSave(); });
  server->on("/api/wifi/list", HTTP_GET, [this] { handleWifiList(); });
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleWifiDelete(); });

  // Font management endpoints
  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
  server->on("/api/fonts/upload", HTTP_POST, [this] { handleFontUpload(); }, [this] { handleFontUploadData(); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); });

  // OPDS server endpoints
  server->on("/api/opds", HTTP_GET, [this] { handleGetOpdsServers(); });
  server->on("/api/opds", HTTP_POST, [this] { handlePostOpdsServer(); });
  server->on("/api/opds/delete", HTTP_POST, [this] { handleDeleteOpdsServer(); });

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

  // Collect WebDAV protocol headers before registering the handler.
  const char* webHeaders[] = {"Depth", "Destination", "Overwrite", "If", "Lock-Token", "Timeout"};
  server->collectHeaders(webHeaders, 6);
  server->addHandler(new WebDAVHandler());  // WebServer owns and deletes the handler on stop
  LOG_DBG("WEB", "WebDAV handler initialized");

  server->begin();

  // Start WebSocket server for fast binary uploads
  LOG_DBG("WEB", "Starting WebSocket server on port %d...", wsPort);
  wsServer.reset(new WebSocketsServer(wsPort));
  wsInstance = const_cast<CrossPointWebServer*>(this);
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);
  LOG_DBG("WEB", "WebSocket server started");

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  // All request handlers run on the task that calls handleClient(). Register
  // that task before any handler can call esp_task_wdt_reset().
  const esp_err_t watchdogResult = esp_task_wdt_add(nullptr);
  watchdogTaskRegistered = watchdogResult == ESP_OK;
  if (!watchdogTaskRegistered) {
    LOG_ERR("WEB", "Failed to register web server task with watchdog: %s", esp_err_to_name(watchdogResult));
  }

  running = true;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "WebSocket at ws://%s:%d/", ipAddr.c_str(), wsPort);
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  // Explicit close() required: file-scope global persists beyond function scope
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
}

void CrossPointWebServer::stop() {
  if (fontUpload.transactionActive) {
    abortFontUploadBatch("Font upload interrupted by server stop");
  } else if (fontUpload.cleanupPending && !fontUpload.familyName.empty()) {
    if (FontInstaller::cleanupCommittedFamilyInstall(fontUpload.familyName.c_str())) {
      fontUpload.cleanupPending = false;
    }
  } else if (fontUpload.file.isOpen()) {
    fontUpload.file.close();
    if (!fontUpload.tempPath.empty() && Storage.exists(fontUpload.tempPath.c_str())) {
      Storage.remove(fontUpload.tempPath.c_str());
    }
    fontUpload.file = FsFile();
  }

  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    if (watchdogTaskRegistered) {
      esp_task_wdt_delete(nullptr);
      watchdogTaskRegistered = false;
    }
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress && wsUploadFile) {
    abortWsUpload("WEB");
  }

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
    LOG_DBG("WEB", "WebSocket server stopped");
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());

  if (watchdogTaskRegistered) {
    esp_task_wdt_delete(nullptr);
    watchdogTaskRegistered = false;
  }

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  if (fontUpload.transactionActive && millis() - fontUpload.lastActivityAt >= FontUploadState::BATCH_IDLE_TIMEOUT_MS) {
    LOG_ERR("WEB", "Font upload family batch timed out: %s", fontUpload.familyName.c_str());
    abortFontUploadBatch("Font upload batch timed out");
  }
  if (fontUpload.cleanupPending && !fontUpload.familyName.empty() &&
      millis() - fontUpload.lastCleanupAttemptAt >= FontUploadState::CLEANUP_RETRY_INTERVAL_MS) {
    fontUpload.lastCleanupAttemptAt = millis();
    if (FontInstaller::cleanupCommittedFamilyInstall(fontUpload.familyName.c_str())) {
      fontUpload.cleanupPending = false;
      LOG_INF("WEB", "Finished deferred font family cleanup: %s", fontUpload.familyName.c_str());
    }
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  server->handleClient();
  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

static void sendHtmlContent(WebServer* server, const char* data, size_t len) {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", data, len);
}

void CrossPointWebServer::handleRoot() const {
  sendHtmlContent(server.get(), HomePageHtml, HomePageHtmlCompressedSize);
  LOG_DBG("WEB", "Served root page");
}

void CrossPointWebServer::handleJszip() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "application/javascript", jszip_minJs, jszip_minJsCompressedSize);
  LOG_DBG("WEB", "Served jszip.min.js");
}

void CrossPointWebServer::handleNotFound() const {
  // CORS preflight: routes are registered per-method, so OPTIONS requests land
  // here. The Access-Control-Allow-* headers are added by enableCORS().
  if (server->method() == HTTP_OPTIONS) {
    server->send(204, "text/plain", "");
    return;
  }

  // in AP mode, redirect unmatched browser/captive-portal requests to "/" so the OS auto-opens the browser
  // API requests (/api/*) still return 404 so XHR errors surface correctly
  // see https://en.wikipedia.org/wiki/Captive_portal#Detection
  if (apMode && !server->uri().startsWith("/api/")) {
    server->sendHeader("Location", "/", true);
    server->send(302, "text/plain", "");
    return;
  }

  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = CROSSPOINT_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";

  char snBuf[33] = {0};
  bool valid = false;
#if !CONFIG_IDF_TARGET_ESP32
  // Classic ESP32's efuse table has no USER_DATA block (C3/S3 only)
  if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, snBuf, 256) == ESP_OK) {
    valid = snBuf[0] != '\0' && snBuf[0] != (char)0xFF;
    for (int i = 0; i < 32 && snBuf[i] != '\0'; i++) {
      if (!std::isprint(static_cast<unsigned char>(snBuf[i]))) {
        valid = false;
        break;
      }
    }
  }
#endif

  if (valid) {
    doc["serial"] = snBuf;
  } else {
    doc["serial"] = "Not found";
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::scanFiles(const char* path, const std::function<bool(const FileInfo&)>& callback) const {
  FsFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  LOG_DBG("WEB", "Scanning files in: %s", path);

  FsFile file = root.openNextFile();
  while (file) {
    FileInfo info;
    if (!file.getName(info.name, sizeof(info.name))) {
      LOG_DBG("WEB", "Skipping file entry with invalid name in: %s", path);
      file.close();
      yield();
      resetTaskWatchdogIfSubscribed();
      file = root.openNextFile();
      continue;
    }

    auto fileName = String(info.name);

    // Skip hidden items (starting with ".") unless the device setting asks to show them.
    bool shouldHide = !SETTINGS.showHiddenFiles && fileName.startsWith(".");
    if (!shouldHide && StoragePathPolicy::isProtectedItemName(fileName)) {
      shouldHide = true;
    }

    if (shouldHide) {
      file.close();
      yield();
      resetTaskWatchdogIfSubscribed();
      file = root.openNextFile();
      continue;
    }

    if (!isProtectedItemName(info.name)) {
      info.isDirectory = file.isDirectory();
      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = endsWithIgnoreCase(info.name, ".epub");
      }

      if (!callback(info)) {
        file.close();
        break;
      }
    }

    file.close();
    yield();                          // Yield to allow WiFi and other tasks to process during long scans
    resetTaskWatchdogIfSubscribed();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  sendHtmlContent(server.get(), FilesPageHtml, FilesPageHtmlCompressedSize);
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = normalizeWebPath(server->arg("path"));
  }

  if (isProtectedWebPath(currentPath)) {
    server->send(403, "application/json", "[]");
    return;
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char output[512];
  char escapedName[FileInfo::NAME_BUFFER_SIZE * 2];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  size_t listedEntries = 0;
  bool listingTruncated = false;

  scanFiles(currentPath.c_str(), [this, &output, &escapedName, &listedEntries, &listingTruncated,
                                  seenFirst](const FileInfo& info) mutable {
    if (listedEntries >= MAX_WEB_LIST_ENTRIES) {
      listingTruncated = true;
      return false;
    }

    if (appendEscapedJsonString(escapedName, sizeof(escapedName), info.name) == 0 && info.name[0] != '\0') {
      LOG_DBG("WEB", "Skipping file entry with oversized escaped JSON name");
      return true;
    }

    const int written = snprintf(output, outputSize, "{\"name\":\"%s\",\"size\":%llu,\"isDirectory\":%s,\"isEpub\":%s}",
                                 escapedName, static_cast<unsigned long long>(info.size),
                                 info.isDirectory ? "true" : "false", info.isEpub ? "true" : "false");
    if (written < 0 || static_cast<size_t>(written) >= outputSize) {
      LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name);
      return true;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
    listedEntries++;
    return true;
  });
  server->sendContent("]");
  server->sendContent("");
  if (listingTruncated) {
    LOG_DBG("WEB", "File listing truncated at %u entries for path: %s", static_cast<unsigned>(MAX_WEB_LIST_ENTRIES),
            currentPath.c_str());
  }
  LOG_DBG("WEB", "Served file listing page for path: %s", currentPath.c_str());
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server->arg("path");
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  itemPath = normalizeWebPath(itemPath);
  if (isProtectedWebPath(itemPath)) {
    server->send(403, "text/plain", "Cannot access protected items");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  const size_t chunkSize = 4096;
  uint8_t buffer[chunkSize];

  bool downloadOk = true;
  while (downloadOk && file.available()) {
    int result = file.read(buffer, chunkSize);
    if (result <= 0) break;
    size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      resetTaskWatchdogIfSubscribed();
      size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        downloadOk = false;
        break;
      }
      totalWritten += wrote;
    }
  }
  client.clear();
  file.close();
}

// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    resetTaskWatchdogIfSubscribed();  // Reset watchdog before potentially slow SD write
    const unsigned long writeStart = millis();
    const size_t written = state.file.write(state.buffer.get(), state.bufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    resetTaskWatchdogIfSubscribed();  // Reset watchdog after SD write

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}
bool CrossPointWebServer::allocateHttpUploadBuffer(UploadState& state) const {
  if (state.buffer) return true;
  state.buffer = makeUniqueNoThrow<uint8_t[]>(UploadState::UPLOAD_BUFFER_SIZE);
  if (state.buffer) return true;
  LOG_ERR("WEB", "OOM: HTTP upload buffer (%u bytes), free=%u max=%u",
          static_cast<unsigned>(UploadState::UPLOAD_BUFFER_SIZE), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  return false;
}

static void abortHttpUpload(CrossPointWebServer::UploadState& state, const String& error) {
  state.error = error;
  state.bufferPos = 0;
  state.buffer.reset();
  if (state.file) {
    state.file.close();
    if (!state.fileName.isEmpty()) {
      const String filePath = buildChildPath(state.path, state.fileName);
      Storage.remove(filePath.c_str());
    }
  }
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  static size_t lastLoggedSize = 0;

  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  resetTaskWatchdogIfSubscribed();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();
  if (upload.status == UPLOAD_FILE_START) {
    // Reset watchdog - this is the critical 1% crash point
    resetTaskWatchdogIfSubscribed();

    state.fileName = upload.filename;
    state.size = 0;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    lastLoggedSize = 0;
    state.bufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = normalizeWebPath(server->arg("path"));
    } else {
      state.path = "/";
    }

    if (isProtectedWebPath(state.path) || isProtectedItemName(state.fileName)) {
      state.error = "Cannot upload to protected path";
      LOG_DBG("WEB", "[UPLOAD] Rejected protected upload: %s to %s", state.fileName.c_str(), state.path.c_str());
      return;
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    // Create file path
    String filePath = buildChildPath(state.path, state.fileName);

    // Check if file already exists - SD operations can be slow
    resetTaskWatchdogIfSubscribed();
    if (Storage.exists(filePath.c_str())) {
      LOG_DBG("WEB", "[UPLOAD] Overwriting existing file: %s", filePath.c_str());
      resetTaskWatchdogIfSubscribed();
      Storage.remove(filePath.c_str());
    }

    if (!storageHasSpaceForUpload(0)) {
      state.error = "Not enough free space";
      LOG_DBG("WEB", "[UPLOAD] Rejected upload without storage reserve: %s", state.fileName.c_str());
      return;
    }
    if (!allocateHttpUploadBuffer(state)) {
      state.error = "Insufficient memory for upload";
      return;
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    resetTaskWatchdogIfSubscribed();
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }
    resetTaskWatchdogIfSubscribed();

    LOG_DBG("WEB", "[UPLOAD] File created successfully: %s", filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (state.file && state.error.isEmpty()) {
      if (uploadTimedOut(uploadStartTime)) {
        abortHttpUpload(state, "Upload timed out");
        LOG_DBG("WEB", "[UPLOAD] Rejected timed out upload: %s", state.fileName.c_str());
        return;
      }
      if (uploadWouldExceedLimit(state.size, upload.currentSize)) {
        abortHttpUpload(state, "Upload exceeds maximum size");
        LOG_DBG("WEB", "[UPLOAD] Rejected oversized upload: %s", state.fileName.c_str());
        return;
      }
      if (!storageHasSpaceForUpload(state.bufferPos + upload.currentSize)) {
        abortHttpUpload(state, "Not enough free space");
        LOG_DBG("WEB", "[UPLOAD] Rejected upload without enough free space: %s", state.fileName.c_str());
        return;
      }

      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.get() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            state.error = "Failed to write to SD card - disk may be full";
            state.file.close();
            return;
          }
        }
      }

      state.size += upload.currentSize;

      // Log progress every 100KB
      if (state.size - lastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", state.size, state.size / 1024.0, kbps,
                writeCount);
        lastLoggedSize = state.size;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      state.file.close();

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = buildChildPath(state.path, state.fileName);
        clearBookCache(filePath.c_str());
      }
    }
    state.buffer.reset();
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) {
      state.file.close();
      // Try to delete the incomplete file
      String filePath = buildChildPath(state.path, state.fileName);
      Storage.remove(filePath.c_str());
    }
    state.buffer.reset();
    state.error = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    if (error == "Upload exceeds maximum size") {
      server->send(413, "text/plain", error);
    } else if (error == "Upload timed out") {
      server->send(408, "text/plain", error);
    } else if (error == "Not enough free space") {
      server->send(507, "text/plain", error);
    } else {
      server->send(400, "text/plain", error);
    }
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = server->arg("name");

  // Validate folder name
  if (folderName.isEmpty()) {
    server->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = normalizeWebPath(server->arg("path"));
  }

  if (isProtectedWebPath(parentPath) || isProtectedItemName(folderName)) {
    server->send(403, "text/plain", "Cannot create protected folder");
    return;
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = server->arg("name");
  newName.trim();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (isProtectedWebPath(itemPath)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0) {
    server->send(400, "text/plain", "Invalid file name");
    return;
  }
  if (isProtectedItemName(newName)) {
    server->send(403, "text/plain", "Cannot rename to protected name");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;
  if (isProtectedWebPath(newPath)) {
    server->send(403, "text/plain", "Cannot rename to protected path");
    return;
  }

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }
  if (isProtectedWebPath(itemPath) || isProtectedWebPath(destPath)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  FsFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = p.as<String>();

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Ensure path starts with /
    if (!itemPath.startsWith("/")) {
      itemPath = "/" + itemPath;
    }

    if (isProtectedWebPath(itemPath)) {
      failedItems += itemPath + " (protected file); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    FsFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      // For folders, ensure empty before removing
      FsFile entry = f.openNextFile();
      if (entry) {
        entry.close();
        f.close();
        failedItems += itemPath + " (folder not empty); ";
        allSuccess = false;
        continue;
      }
      f.close();
      success = Storage.rmdir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
      clearBookCache(itemPath.c_str());
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendHtmlContent(server.get(), SettingsPageHtml, SettingsPageHtmlCompressedSize);
  LOG_DBG("WEB", "Served settings page");
}

void CrossPointWebServer::buildSettingsCache() const {
  const auto& settings = getBaseSettingsList();

  static char output[2048];
  constexpr size_t outputSize = sizeof(output);
  std::string response;
  response.reserve(8192);
  auto sendChunk = [&response](const char* data, size_t len) {
    response.append(data, len);
    return true;
  };
  sendChunk("[", 1);
  bool seenFirst = false;
  JsonDocument doc;
  for (const auto& s : settings) {
    resetTaskWatchdogIfSubscribed();  // SD font scan + serialization must not trip the WDT
    if (!s.key) continue;             // Skip ACTION-only entries

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        JsonArray options = doc["options"].to<JsonArray>();

        if (isReaderFontFamilySetting(s)) {
          // Use the already-scanned FontMgr registry populated at boot.
          // Rescanning synchronously can hang if SD I/O stalls.
          const int builtinCount = static_cast<int>(s.enumValues.size());
          const int selectedExternal = FontMgr.getSelectedIndex();
          if (selectedExternal >= 0) {
            doc["value"] = builtinCount + selectedExternal;
          } else if (s.valuePtr) {
            doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
          } else {
            doc["value"] = 0;
          }

          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt));
          }

          for (int i = 0; i < FontMgr.getFontCount(); i++) {
            const FontInfo* info = FontMgr.getFontInfo(i);
            if (!info) continue;
            std::string label = buildExternalFontLabel(info->filename, info->name, info->size,
                                                       ExternalFont::canFitGlyph(info->width, info->height));
            options.add(label);
          }
        } else {
          if (s.valuePtr) {
            doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
          } else if (s.valueGetter) {
            doc["value"] = static_cast<int>(s.valueGetter());
          }
          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.obfuscated) {
          doc["secret"] = true;
          if (s.stringGetter) {
            doc["configured"] = !s.stringGetter().empty();
          } else if (s.stringMaxLen > 0 && s.stringOffset) {
            const char* strPtr = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
            doc["configured"] = strPtr[0] != '\0';
          } else {
            doc["configured"] = false;
          }
        } else {
          if (s.stringGetter) {
            doc["value"] = s.stringGetter();
          } else if (s.stringMaxLen > 0 && s.stringOffset) {
            const char* strPtr = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
            doc["value"] = std::string(strPtr);
          }
        }
        break;
      }
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    if (seenFirst) {
      if (!sendChunk(",", 1)) {
        LOG_DBG("WEB", "Settings client stopped reading response");
        return;
      }
    } else {
      seenFirst = true;
    }
    if (!sendChunk(output, written)) {
      LOG_DBG("WEB", "Settings client stopped reading response");
      return;
    }
  }

  // Add UI font selector for web settings.
  // Device UI already has a dedicated action page for this; web gets an enum list.
  doc.clear();
  doc["key"] = "uiFontFamily";
  doc["name"] = I18N.get(StrId::STR_EXT_UI_FONT);
  doc["category"] = I18N.get(StrId::STR_CAT_DISPLAY);
  doc["type"] = "enum";
  const int selectedUiExternal = FontMgr.getUiSelectedIndex();
  doc["value"] = selectedUiExternal >= 0 ? selectedUiExternal + 1 : 0;

  JsonArray uiOptions = doc["options"].to<JsonArray>();
  uiOptions.add(I18N.get(StrId::STR_BUILTIN_DISABLED));
  for (int i = 0; i < FontMgr.getFontCount(); i++) {
    const FontInfo* info = FontMgr.getFontInfo(i);
    if (!info) continue;
    std::string label = buildExternalFontLabel(info->filename, info->name, info->size,
                                               ExternalFont::canFitGlyph(info->width, info->height));
    uiOptions.add(label);
  }

  const size_t uiWritten = serializeJson(doc, output, outputSize);
  if (uiWritten < outputSize) {
    if (seenFirst && !sendChunk(",", 1)) {
      LOG_DBG("WEB", "Settings client stopped reading response");
      return;
    }
    seenFirst = true;
    if (!sendChunk(output, uiWritten)) {
      LOG_DBG("WEB", "Settings client stopped reading response");
      return;
    }
  } else {
    LOG_DBG("WEB", "Skipping oversized setting JSON for: uiFontFamily");
  }

  // Add language selector for web settings.
  doc.clear();
  doc["key"] = "language";
  doc["name"] = I18N.get(StrId::STR_LANGUAGE);
  doc["category"] = I18N.get(StrId::STR_CAT_SYSTEM);
  doc["type"] = "enum";
  doc["value"] = static_cast<int>(I18N.getLanguage());

  JsonArray languageOptions = doc["options"].to<JsonArray>();
  for (int i = 0; i < static_cast<int>(getLanguageCount()); i++) {
    languageOptions.add(I18N.getLanguageName(static_cast<Language>(i)));
  }

  const size_t langWritten = serializeJson(doc, output, outputSize);
  if (langWritten < outputSize) {
    if (seenFirst && !sendChunk(",", 1)) {
      LOG_DBG("WEB", "Settings client stopped reading response");
      return;
    }
    if (!sendChunk(output, langWritten)) {
      LOG_DBG("WEB", "Settings client stopped reading response");
      return;
    }
  } else {
    LOG_DBG("WEB", "Skipping oversized setting JSON for: language");
  }

  sendChunk("]", 1);
  auto* safeServer = static_cast<WdtSafeWebServer*>(server.get());
  safeServer->beginWdtSafeResponse("application/json", response.size());
  if (!safeServer->writeFailed()) {
    safeServer->writeWdtSafeContent(response.data(), response.size());
  }
  if (safeServer->writeFailed()) {
    LOG_DBG("WEB", "Settings client stopped reading response");
    return;
  }
  LOG_DBG("WEB", "Served settings API (%zu bytes)", response.size());
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  const auto& settings = getBaseSettingsList();
  int applied = 0;

  const bool hasUiFontRequest = doc["uiFontFamily"].is<JsonVariant>();
  bool hasReaderFontRequest = false;
  const SettingInfo* readerFontSetting = nullptr;
  for (const auto& s : settings) {
    if (isReaderFontFamilySetting(s) && doc[s.key].is<JsonVariant>()) {
      hasReaderFontRequest = true;
      readerFontSetting = &s;
      break;
    }
  }

  if (hasUiFontRequest || hasReaderFontRequest) {
    // Use already-scanned registry from boot; skip sync rescan to avoid SD stalls.

    const int oldReaderIndex = FontMgr.getSelectedIndex();
    const int oldUiIndex = FontMgr.getUiSelectedIndex();
    int requestedReaderIndex = (oldReaderIndex < 0 || FontMgr.getFontInfo(oldReaderIndex)) ? oldReaderIndex : -1;
    int requestedUiIndex = (oldUiIndex < 0 || FontMgr.getFontInfo(oldUiIndex)) ? oldUiIndex : -1;
    int requestedBuiltinReaderFamily = -1;
    bool fontRequestValid = true;

    if (hasUiFontRequest) {
      const int val = doc["uiFontFamily"].as<int>();
      if (val == 0) {
        requestedUiIndex = -1;
      } else {
        const int externalIndex = val - 1;
        const FontInfo* info = FontMgr.getFontInfo(externalIndex);
        if (!info || !ExternalFont::canFitGlyph(info->width, info->height)) {
          fontRequestValid = false;
        } else {
          requestedUiIndex = externalIndex;
        }
      }
    }

    if (hasReaderFontRequest) {
      const int val = doc[readerFontSetting->key].as<int>();
      const int builtinCount = static_cast<int>(readerFontSetting->enumValues.size());
      if (val >= 0 && val < builtinCount) {
        requestedReaderIndex = -1;
        requestedBuiltinReaderFamily = val;
      } else {
        const int externalIndex = val - builtinCount;
        const FontInfo* info = FontMgr.getFontInfo(externalIndex);
        if (!info || !ExternalFont::canFitGlyph(info->width, info->height)) {
          fontRequestValid = false;
        } else {
          requestedReaderIndex = externalIndex;
        }
      }
    }

    if (fontRequestValid && FontMgr.selectFonts(requestedReaderIndex, requestedUiIndex)) {
      if (hasReaderFontRequest) {
        SETTINGS.sdFontFamilyName[0] = '\0';
        if (requestedBuiltinReaderFamily >= 0) {
          SETTINGS.fontFamily = static_cast<uint8_t>(requestedBuiltinReaderFamily);
        }
        applied++;
      }
      if (hasUiFontRequest) {
        SETTINGS.sdUiFontFamilyName[0] = '\0';
        applied++;
      }
    } else if (hasReaderFontRequest || hasUiFontRequest) {
      LOG_ERR("WEB", "Font setting transaction failed; keeping previous Reader/UI selection");
    }
  }

  if (doc["language"].is<JsonVariant>()) {
    const int val = doc["language"].as<int>();
    if (val >= 0 && val < static_cast<int>(getLanguageCount())) {
      I18N.setLanguage(static_cast<Language>(val));
      applied++;
    }
  }

  for (const auto& s : settings) {
    if (isUiFontFamilyKey(s.key) || isLanguageSettingKey(s.key)) continue;
    if (!s.key) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        if (isReaderFontFamilySetting(s)) {
          // Reader font requests are applied above together with the optional
          // UI font request, so a two-slot update cannot partially apply.
          break;
        }

        if (val >= 0 && val < static_cast<int>(s.enumValues.size())) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0 && s.stringOffset) {
          char* strPtr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(strPtr, val.c_str(), s.stringMaxLen - 1);
          strPtr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  resetTaskWatchdogIfSubscribed();  // Flash write ahead
  SETTINGS.saveToFile();
  resetTaskWatchdogIfSubscribed();  // Flash write done

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

// ---- OPDS Server API ----

void CrossPointWebServer::handleGetOpdsServers() const {
  const auto& servers = OPDS_STORE.getServers();

  std::string response;
  response.reserve(512 + servers.size() * 256);
  auto sendChunk = [&response](const char* data, size_t len) {
    response.append(data, len);
    return true;
  };
  sendChunk("[", 1);

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;
  bool seenFirst = false;

  for (size_t i = 0; i < servers.size(); i++) {
    resetTaskWatchdogIfSubscribed();
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    // Never expose passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !servers[i].password.empty();

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (seenFirst && !sendChunk(",", 1)) return;
    seenFirst = true;
    if (!sendChunk(output, written)) return;
  }

  sendChunk("]", 1);
  auto* safeServer = static_cast<WdtSafeWebServer*>(server.get());
  safeServer->beginWdtSafeResponse("application/json", response.size());
  if (!safeServer->writeFailed()) {
    safeServer->writeWdtSafeContent(response.data(), response.size());
  }
  if (safeServer->writeFailed()) return;
  LOG_DBG("WEB", "Served OPDS servers API (%zu servers)", servers.size());
}

void CrossPointWebServer::handlePostOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = doc["name"] | std::string("");
  opdsServer.url = doc["url"] | std::string("");
  opdsServer.username = doc["username"] | std::string("");

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // we preserve the existing password — the web UI omits it when the user hasn't changed it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    // Preserve existing password if not explicitly provided
    if (!hasPasswordField) {
      const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
      if (existing) password = existing->password;
    }
    opdsServer.password = password;
    OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer);
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    opdsServer.password = password;
    if (!OPDS_STORE.addServer(opdsServer)) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server: %s", opdsServer.name.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }

  OPDS_STORE.removeServer(static_cast<size_t>(idx));
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server->send(200, "text/plain", "OK");
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        abortWsUpload("WS");
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String((char*)payload);
      if (msg.startsWith("START:")) {
        LOG_DBG("WS", "Upload START from client %u", num);
      } else {
        LOG_DBG("WS", "Text from client %u: %s", num, msg.c_str());
      }

      if (msg.startsWith("START:")) {
        // Reject any START while an upload is already active to prevent
        // leaking the open wsUploadFile handle (owning client re-START included)
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = msg.substring(6, firstColon);
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          bool uploadTooLarge = false;
          if (!parseUploadSizeToken(sizeToken, wsUploadSize, uploadTooLarge)) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          if (uploadTooLarge) {
            LOG_DBG("WS", "START rejected: upload too large");
            wsServer->sendTXT(num, "ERROR:Upload exceeds maximum size");
            return;
          }
          wsUploadPath = msg.substring(secondColon + 1);
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsUploadStartTime = millis();

          wsUploadPath = normalizeWebPath(wsUploadPath);
          if (isProtectedWebPath(wsUploadPath) || isProtectedItemName(wsUploadFileName)) {
            wsServer->sendTXT(num, "ERROR:Cannot upload to protected path");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          // Build file path
          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          // Check if file exists and remove it
          resetTaskWatchdogIfSubscribed();
          if (Storage.exists(filePath.c_str())) {
            Storage.remove(filePath.c_str());
          }

          if (!storageHasSpaceForUpload(wsUploadSize)) {
            wsServer->sendTXT(num, "ERROR:Not enough free space");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          // Open file for writing
          resetTaskWatchdogIfSubscribed();
          if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }
          resetTaskWatchdogIfSubscribed();

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            // Explicit close() required: file-scope global persists beyond function scope
            wsUploadFile.close();
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            clearBookCache(filePath.c_str());
            wsServer->sendTXT(num, "DONE");
            wsLastProgressSent = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }
      if (uploadTimedOut(wsUploadStartTime)) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload timed out");
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      if (!storageHasSpaceForUpload(length)) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Not enough free space");
        return;
      }
      resetTaskWatchdogIfSubscribed();
      size_t written = wsUploadFile.write(payload, length);
      resetTaskWatchdogIfSubscribed();

      if (written != length) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        // Explicit close() required: file-scope global persists beyond function scope
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        clearBookCache(filePath.c_str());

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

// --- WiFi credential management API handlers (CJK) ---

void CrossPointWebServer::handleWifiScan() const {
  LOG_DBG("WEB", "WiFi scan requested");

  // In AP mode we need to briefly enable STA to scan, without tearing down the AP.
  const wifi_mode_t prevMode = WiFi.getMode();
  if (apMode) {
    WiFi.mode(WIFI_AP_STA);
    delay(100);
  }

  // Use async scan to avoid long blocking calls that can trigger task watchdog resets.
  const unsigned long scanStart = millis();
  constexpr unsigned long SCAN_TIMEOUT_MS = 20000;
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);

  int n = WIFI_SCAN_RUNNING;
  while (n == WIFI_SCAN_RUNNING && (millis() - scanStart) < SCAN_TIMEOUT_MS) {
    resetTaskWatchdogIfSubscribed();
    delay(20);
    n = WiFi.scanComplete();
  }

  // Restore previous WiFi mode after scan
  if (apMode && prevMode != WIFI_AP_STA) {
    WiFi.mode(prevMode);
  }

  if (n == WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    server->send(500, "application/json", "{\"error\":\"Scan timeout\"}");
    LOG_ERR("WEB", "WiFi scan timed out after %lu ms", millis() - scanStart);
    return;
  }

  if (n < 0) {
    server->send(500, "application/json", "{\"error\":\"Scan failed\"}");
    LOG_ERR("WEB", "WiFi scan failed with code %d", n);
    return;
  }

  // De-duplicate by SSID, keeping the strongest signal
  std::map<String, int> bestIndex;
  for (int i = 0; i < n; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;  // Skip hidden networks
    auto it = bestIndex.find(ssid);
    if (it == bestIndex.end() || WiFi.RSSI(i) > WiFi.RSSI(it->second)) {
      bestIndex[ssid] = i;
    }
  }

  // Build JSON array
  String json = "[";
  bool first = true;
  for (const auto& entry : bestIndex) {
    const int i = entry.second;
    if (!first) json += ",";
    first = false;

    JsonDocument doc;
    doc["ssid"] = WiFi.SSID(i);
    doc["rssi"] = WiFi.RSSI(i);
    doc["encrypted"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    doc["saved"] = WIFI_STORE.hasSavedCredential(WiFi.SSID(i).c_str());

    char buf[256];
    serializeJson(doc, buf, sizeof(buf));
    json += buf;
  }
  json += "]";

  WiFi.scanDelete();
  server->send(200, "application/json", json);
  LOG_DBG("WEB", "WiFi scan returned %d unique networks", bestIndex.size());
}

void CrossPointWebServer::handleWifiSave() const {
  // Expect JSON body: {"ssid": "...", "password": "..."}
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"Missing request body\"}");
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  const char* ssid = doc["ssid"];
  const char* password = doc["password"];

  if (!ssid || strlen(ssid) == 0) {
    server->send(400, "application/json", "{\"error\":\"SSID is required\"}");
    return;
  }
  if (!password) {
    server->send(400, "application/json", "{\"error\":\"Password is required\"}");
    return;
  }

  // Load existing credentials, add/update, then save
  WIFI_STORE.loadFromFile();
  const bool ok = WIFI_STORE.addCredential(ssid, password);

  if (ok) {
    server->send(200, "application/json", "{\"success\":true}");
    LOG_DBG("WEB", "WiFi credential saved for SSID: %s", ssid);
  } else {
    server->send(500, "application/json", "{\"error\":\"Failed to save credential\"}");
    LOG_ERR("WEB", "Failed to save WiFi credential for SSID: %s", ssid);
  }
}

void CrossPointWebServer::handleWifiList() const {
  WIFI_STORE.loadFromFile();
  const auto& creds = WIFI_STORE.getCredentials();

  String json = "[";
  for (size_t i = 0; i < creds.size(); i++) {
    if (i > 0) json += ",";
    // Only expose SSID, never the password
    json += "{\"ssid\":\"";
    // Escape any quotes in SSID
    String escaped = creds[i].ssid.c_str();
    escaped.replace("\"", "\\\"");
    json += escaped;
    json += "\"}";
  }
  json += "]";

  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleWifiDelete() const {
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"Missing request body\"}");
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  const char* ssid = doc["ssid"];
  if (!ssid || strlen(ssid) == 0) {
    server->send(400, "application/json", "{\"error\":\"SSID is required\"}");
    return;
  }

  WIFI_STORE.loadFromFile();
  const bool ok = WIFI_STORE.removeCredential(ssid);

  if (ok) {
    server->send(200, "application/json", "{\"success\":true}");
    LOG_DBG("WEB", "WiFi credential deleted for SSID: %s", ssid);
  } else {
    server->send(404, "application/json", "{\"error\":\"Credential not found\"}");
  }
}

// --- Font management handlers ---

void CrossPointWebServer::handleFontsPage() const {
  sendHtmlContent(server.get(), FontsPageHtml, FontsPageHtmlCompressedSize);
  LOG_DBG("WEB", "Served fonts page");
}

void CrossPointWebServer::handleFontList() const {
  // Pick up any uploads/deletes that happened since the last reader load.
  const_cast<SdCardFontSystem&>(sdFontSystem).refreshIfDirty();
  const auto& families = sdFontSystem.registry().getFamilies();

  JsonDocument doc;
  JsonArray arr = doc["families"].to<JsonArray>();
  doc["maxFamilies"] = SdCardFontRegistry::MAX_SD_FAMILIES;

  for (const auto& family : families) {
    JsonObject fObj = arr.add<JsonObject>();
    fObj["name"] = family.name;

    JsonArray sizes = fObj["sizes"].to<JsonArray>();
    for (uint8_t s : family.availableSizes()) {
      sizes.add(s);
    }

    JsonArray files = fObj["files"].to<JsonArray>();
    for (const auto& file : family.files) {
      JsonObject fileObj = files.add<JsonObject>();
      // Extract filename from full path
      const char* name = strrchr(file.path.c_str(), '/');
      fileObj["name"] = name ? name + 1 : file.path.c_str();

      // Stat the file for size
      FsFile f;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileObj["size"] = static_cast<unsigned long>(f.size());
        f.close();
      } else {
        fileObj["size"] = 0;
      }
    }
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleFontUploadData() {
  HTTPUpload& upload = server->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      resetTaskWatchdogIfSubscribed();
      if (fontUpload.requestValid || fontUpload.file.isOpen()) {
        LOG_ERR("WEB", "Replacing an incomplete font upload request");
        abortFontUploadBatch("Font upload request was replaced before completion");
      }

      if (fontUpload.cleanupPending && !fontUpload.familyName.empty()) {
        fontUpload.lastCleanupAttemptAt = millis();
        if (!FontInstaller::cleanupCommittedFamilyInstall(fontUpload.familyName.c_str())) {
          resetFontUploadRequest();
          fontUpload.errorMessage = "Previous font family cleanup is still pending";
          LOG_ERR("WEB", "Cannot start font upload while committed cleanup is pending: %s",
                  fontUpload.familyName.c_str());
          break;
        }
        fontUpload.cleanupPending = false;
      }
      resetFontUploadRequest();

      const String family = server->arg("family");
      const String fingerprintArg = server->arg("fingerprint");
      const String sessionArg = server->arg("session");
      const String indexArg = server->arg("index");
      const String totalArg = server->arg("total");
      const String fileSizeArg = server->arg("size");
      uint32_t fingerprint = 0;
      uint32_t sessionId = 0;
      uint32_t parsedIndex = 0;
      uint32_t parsedTotal = 0;
      uint32_t parsedFileSize = 0;

      if (!FontInstaller::isValidFamilyName(family.c_str())) {
        LOG_ERR("WEB", "Invalid font family name: %s", family.c_str());
        abortFontUploadBatch("Invalid font family name");
        break;
      }
      if (!parseUint32Arg(fingerprintArg, fingerprint) || !parseUint32Arg(sessionArg, sessionId) ||
          !parseUint32Arg(indexArg, parsedIndex) || !parseUint32Arg(totalArg, parsedTotal) ||
          !parseUint32Arg(fileSizeArg, parsedFileSize) || parsedTotal == 0 ||
          parsedTotal > FontUploadState::MAX_FILES_PER_FAMILY || parsedIndex >= parsedTotal) {
        LOG_ERR("WEB", "Invalid font family batch metadata");
        abortFontUploadBatch("Invalid font family batch metadata");
        break;
      }
      if (parsedFileSize < fontUpload.magic.size() || parsedFileSize > FontUploadState::MAX_FILE_SIZE) {
        LOG_ERR("WEB", "Invalid font upload size: %lu", static_cast<unsigned long>(parsedFileSize));
        abortFontUploadBatch("Invalid font upload size");
        break;
      }

      String filename = upload.filename;
      filename.replace(' ', '_');
      if (!FontInstaller::isValidCpfontFilename(filename.c_str())) {
        LOG_ERR("WEB", "Invalid font filename: %s", filename.c_str());
        abortFontUploadBatch("Invalid font filename");
        break;
      }

      const size_t currentIndex = static_cast<size_t>(parsedIndex);
      const size_t totalFiles = static_cast<size_t>(parsedTotal);
      const size_t expectedFileSize = static_cast<size_t>(parsedFileSize);

      const bool matchingBatch = fontUpload.familyName == family.c_str() && fontUpload.fingerprint == fingerprint &&
                                 fontUpload.sessionId == sessionId && fontUpload.totalFiles == totalFiles;
      const bool duplicatePreviousFile =
          matchingBatch && fontUpload.nextIndex > 0 && currentIndex + 1 == fontUpload.nextIndex &&
          fontUpload.lastCompletedFilename == filename.c_str() && fontUpload.lastCompletedFileSize == expectedFileSize;
      if (duplicatePreviousFile && millis() - fontUpload.lastActivityAt <= FontUploadState::DUPLICATE_RETRY_WINDOW_MS &&
          (fontUpload.transactionActive || fontUpload.nextIndex == fontUpload.totalFiles)) {
        fontUpload.currentFilename = filename.c_str();
        fontUpload.currentIndex = currentIndex;
        fontUpload.expectedFileSize = expectedFileSize;
        fontUpload.duplicateRequest = true;
        fontUpload.duplicateCommitted = !fontUpload.transactionActive && fontUpload.nextIndex == fontUpload.totalFiles;
        fontUpload.requestValid = true;
        fontUpload.lastActivityAt = millis();
        LOG_INF("WEB", "Accepting duplicate font upload request: %zu/%zu %s", currentIndex + 1, totalFiles,
                filename.c_str());
        break;
      }

      if (fontUpload.transactionActive && currentIndex == 0) {
        LOG_INF("WEB", "Restarting font family upload batch: %s", fontUpload.familyName.c_str());
        abortFontUploadBatch("Restarting font upload batch");
        resetFontUploadRequest();
      }

      if (fontUpload.transactionActive &&
          (fontUpload.familyName != family.c_str() || fontUpload.fingerprint != fingerprint ||
           fontUpload.sessionId != sessionId || fontUpload.totalFiles != totalFiles ||
           fontUpload.nextIndex != currentIndex)) {
        LOG_ERR("WEB", "Font family upload batch metadata or order mismatch");
        abortFontUploadBatch("Font upload batch metadata or order mismatch");
        break;
      }
      if (!fontUpload.transactionActive && currentIndex != 0) {
        LOG_ERR("WEB", "Font family upload batch must start at index zero");
        abortFontUploadBatch("Font upload batch must start at index zero");
        break;
      }

      FontInstaller installer(sdFontSystem.registry());
      char path[160];
      if (!FontInstaller::buildFontPath(family.c_str(), filename.c_str(), path, sizeof(path))) {
        LOG_ERR("WEB", "Invalid or oversized font upload path");
        abortFontUploadBatch("Invalid or oversized font upload path");
        break;
      }
      fontUpload.finalPath = path;
      fontUpload.tempPath = fontUpload.finalPath + ".part";

      if (Storage.exists(fontUpload.tempPath.c_str()) && !Storage.remove(fontUpload.tempPath.c_str())) {
        LOG_ERR("WEB", "Failed to remove stale font upload: %s", fontUpload.tempPath.c_str());
        abortFontUploadBatch("Failed to remove stale font upload");
        break;
      }
      const std::string legacyBackupPath = fontUpload.finalPath + ".bak";
      if (Storage.exists(legacyBackupPath.c_str())) {
        if (Storage.exists(fontUpload.finalPath.c_str())) {
          if (!Storage.remove(legacyBackupPath.c_str())) {
            LOG_ERR("WEB", "Failed to remove stale font backup: %s", legacyBackupPath.c_str());
            abortFontUploadBatch("Failed to remove stale font backup");
            break;
          }
        } else if (!Storage.rename(legacyBackupPath.c_str(), fontUpload.finalPath.c_str())) {
          LOG_ERR("WEB", "Failed to restore interrupted font upload: %s", fontUpload.finalPath.c_str());
          abortFontUploadBatch("Failed to restore interrupted font upload");
          break;
        }
      }

      if (!fontUpload.transactionActive) {
        if (!installer.ensureFamilyDir(family.c_str())) {
          LOG_ERR("WEB", "Failed to create font family dir");
          abortFontUploadBatch("Failed to create font family directory");
          break;
        }
        if (!FontInstaller::beginFamilyInstall(family.c_str(), fingerprint)) {
          LOG_ERR("WEB", "Failed to begin font family transaction: %s", family.c_str());
          abortFontUploadBatch("Failed to begin font family transaction");
          break;
        }
        fontUpload.familyName = family.c_str();
        fontUpload.fingerprint = fingerprint;
        fontUpload.sessionId = sessionId;
        fontUpload.nextIndex = 0;
        fontUpload.totalFiles = totalFiles;
        fontUpload.transactionActive = true;
        fontUpload.lastCompletedFilename.clear();
        fontUpload.lastCompletedFileSize = 0;
      }

      if (!storageHasSpaceForUpload(expectedFileSize)) {
        LOG_ERR("WEB", "Not enough free space for font upload: %lu bytes",
                static_cast<unsigned long>(expectedFileSize));
        abortFontUploadBatch("Not enough free space for font upload");
        break;
      }

      if (!allocateFontUploadBuffer()) {
        abortFontUploadBatch("Insufficient memory for font upload");
        break;
      }
      if (!Storage.openFileForWrite("WEB", fontUpload.tempPath.c_str(), fontUpload.file)) {
        LOG_ERR("WEB", "Failed to open font upload for write: %s", fontUpload.tempPath.c_str());
        abortFontUploadBatch("Failed to open font upload for writing");
        break;
      }

      fontUpload.currentFilename = filename.c_str();
      fontUpload.currentIndex = currentIndex;
      fontUpload.expectedFileSize = expectedFileSize;
      fontUpload.requestValid = true;
      fontUpload.lastActivityAt = millis();
      LOG_DBG("WEB", "Font upload started: %zu/%zu %s -> %s", currentIndex + 1, totalFiles, filename.c_str(),
              fontUpload.tempPath.c_str());
      break;
    }

    case UPLOAD_FILE_WRITE: {
      if (!fontUpload.requestValid) break;
      resetTaskWatchdogIfSubscribed();
      fontUpload.lastActivityAt = millis();

      const size_t bufferedTotal = fontUpload.bytesWritten + fontUpload.bufferPos;
      if (bufferedTotal > fontUpload.expectedFileSize ||
          upload.currentSize > fontUpload.expectedFileSize - bufferedTotal) {
        LOG_ERR("WEB", "Font upload exceeds declared size");
        abortFontUploadBatch("Font upload exceeds declared size");
        break;
      }
      if (!fontUpload.duplicateRequest && !storageHasSpaceForUpload(fontUpload.bufferPos + upload.currentSize)) {
        LOG_ERR("WEB", "Font upload exhausted the storage reserve");
        abortFontUploadBatch("Not enough free space for font upload");
        break;
      }

      if (!fontUpload.magicChecked) {
        const size_t magicRemaining = fontUpload.magic.size() - fontUpload.magicBytes;
        const size_t magicChunk = upload.currentSize < magicRemaining ? upload.currentSize : magicRemaining;
        if (magicChunk > 0) {
          memcpy(fontUpload.magic.data() + fontUpload.magicBytes, upload.buf, magicChunk);
          fontUpload.magicBytes += magicChunk;
        }
        if (fontUpload.magicBytes == fontUpload.magic.size()) {
          fontUpload.magicChecked = true;
          if (memcmp(fontUpload.magic.data(), "CPFONT\0\0", fontUpload.magic.size()) != 0) {
            LOG_ERR("WEB", "Invalid .cpfont magic bytes");
            abortFontUploadBatch("Invalid .cpfont magic bytes");
            break;
          }
        }
      }

      if (fontUpload.duplicateRequest) {
        fontUpload.bytesWritten += upload.currentSize;
        break;
      }

      size_t remaining = upload.currentSize;
      const uint8_t* src = upload.buf;
      while (remaining > 0 && fontUpload.requestValid) {
        const size_t space = FontUploadState::BUFFER_SIZE - fontUpload.bufferPos;
        const size_t chunk = remaining < space ? remaining : space;
        memcpy(fontUpload.buffer.get() + fontUpload.bufferPos, src, chunk);
        fontUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        if (fontUpload.bufferPos == FontUploadState::BUFFER_SIZE) {
          const size_t pending = fontUpload.bufferPos;
          const size_t written = fontUpload.file.write(fontUpload.buffer.get(), pending);
          fontUpload.bytesWritten += written;
          fontUpload.bufferPos = 0;
          if (written != pending) {
            LOG_ERR("WEB", "Failed to write font upload: %zu/%zu bytes", written, pending);
            abortFontUploadBatch("Failed to write font upload");
            break;
          }
          resetTaskWatchdogIfSubscribed();
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      if (!fontUpload.requestValid) break;
      fontUpload.lastActivityAt = millis();
      if (fontUpload.duplicateRequest) {
        if (!fontUpload.magicChecked || fontUpload.bytesWritten != fontUpload.expectedFileSize) {
          LOG_ERR("WEB", "Duplicate font upload does not match declared size or format");
          abortFontUploadBatch("Duplicate font upload does not match declared size or format");
          break;
        }
        fontUpload.requestValid = false;
        fontUpload.requestSucceeded = true;
        fontUpload.requestCommitted = fontUpload.duplicateCommitted;
        LOG_INF("WEB", "Duplicate font upload request verified: %zu/%zu %s", fontUpload.currentIndex + 1,
                fontUpload.totalFiles, fontUpload.currentFilename.c_str());
        break;
      }
      if (fontUpload.bufferPos > 0) {
        const size_t pending = fontUpload.bufferPos;
        const size_t written = fontUpload.file.write(fontUpload.buffer.get(), pending);
        fontUpload.bytesWritten += written;
        fontUpload.bufferPos = 0;
        if (written != pending) {
          LOG_ERR("WEB", "Failed to flush font upload: %zu/%zu bytes", written, pending);
          abortFontUploadBatch("Failed to flush font upload");
          break;
        }
      }
      if (!fontUpload.magicChecked || fontUpload.bytesWritten < fontUpload.magic.size()) {
        LOG_ERR("WEB", "Font upload is shorter than the .cpfont header");
        abortFontUploadBatch("Font upload is shorter than the .cpfont header");
        break;
      }
      if (fontUpload.bytesWritten != fontUpload.expectedFileSize) {
        LOG_ERR("WEB", "Font upload size mismatch: %zu/%zu bytes", fontUpload.bytesWritten,
                fontUpload.expectedFileSize);
        abortFontUploadBatch("Font upload size does not match declared size");
        break;
      }
      fontUpload.file.flush();
      if (!fontUpload.file.close()) {
        LOG_ERR("WEB", "Failed to close font upload: %s", fontUpload.tempPath.c_str());
        abortFontUploadBatch("Failed to close font upload");
        break;
      }
      fontUpload.file = FsFile();

      FontInstaller installer(sdFontSystem.registry());
      if (!installer.validateCpfontFile(fontUpload.tempPath.c_str())) {
        LOG_ERR("WEB", "Uploaded .cpfont failed validation: %s", fontUpload.tempPath.c_str());
        abortFontUploadBatch("Uploaded .cpfont failed validation");
        break;
      }
      if (!FontInstaller::prepareFontReplacement(fontUpload.finalPath.c_str())) {
        LOG_ERR("WEB", "Failed to prepare font replacement: %s", fontUpload.finalPath.c_str());
        abortFontUploadBatch("Failed to prepare font replacement");
        break;
      }
      if (!Storage.rename(fontUpload.tempPath.c_str(), fontUpload.finalPath.c_str())) {
        LOG_ERR("WEB", "Failed to install uploaded font: %s", fontUpload.finalPath.c_str());
        abortFontUploadBatch("Failed to install uploaded font");
        break;
      }

      fontUpload.requestValid = false;
      fontUpload.requestSucceeded = true;
      fontUpload.lastCompletedFilename = fontUpload.currentFilename;
      fontUpload.lastCompletedFileSize = fontUpload.expectedFileSize;
      fontUpload.nextIndex = fontUpload.currentIndex + 1;
      if (fontUpload.nextIndex == fontUpload.totalFiles) {
        if (!FontInstaller::commitFamilyInstall(fontUpload.familyName.c_str())) {
          LOG_ERR("WEB", "Failed to commit font family transaction: %s", fontUpload.familyName.c_str());
          abortFontUploadBatch("Failed to commit font family transaction");
          break;
        }

        fontUpload.transactionActive = false;
        fontUpload.requestCommitted = true;
        fontUpload.cleanupPending = true;
        fontUpload.lastCleanupAttemptAt = millis();
        if (FontInstaller::cleanupCommittedFamilyInstall(fontUpload.familyName.c_str())) {
          fontUpload.cleanupPending = false;
        } else {
          LOG_ERR("WEB", "Committed font family cleanup deferred until recovery: %s", fontUpload.familyName.c_str());
        }
      }

      fontUpload.buffer.reset();
      LOG_DBG("WEB", "Font upload end: %zu/%zu, %zu bytes, committed=%d", fontUpload.nextIndex, fontUpload.totalFiles,
              fontUpload.bytesWritten, fontUpload.requestCommitted);
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      LOG_DBG("WEB", "Font upload aborted");
      if (fontUpload.requestValid || fontUpload.transactionActive) {
        abortFontUploadBatch("Font upload aborted");
      } else if (fontUpload.errorMessage.empty()) {
        fontUpload.errorMessage = "Font upload aborted";
      }
      break;
    }
  }
}

bool CrossPointWebServer::allocateFontUploadBuffer() {
  if (fontUpload.buffer) return true;
  fontUpload.buffer = makeUniqueNoThrow<uint8_t[]>(FontUploadState::BUFFER_SIZE);
  if (fontUpload.buffer) return true;
  LOG_ERR("WEB", "OOM: font upload buffer (%u bytes), free=%u max=%u",
          static_cast<unsigned>(FontUploadState::BUFFER_SIZE), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  return false;
}

void CrossPointWebServer::resetFontUploadRequest() {
  if (fontUpload.file.isOpen()) fontUpload.file.close();
  fontUpload.file = FsFile();
  fontUpload.finalPath.clear();
  fontUpload.tempPath.clear();
  fontUpload.errorMessage.clear();
  fontUpload.currentFilename.clear();
  fontUpload.currentIndex = 0;
  fontUpload.expectedFileSize = 0;
  fontUpload.requestValid = false;
  fontUpload.requestSucceeded = false;
  fontUpload.requestCommitted = false;
  fontUpload.duplicateRequest = false;
  fontUpload.duplicateCommitted = false;
  fontUpload.magicChecked = false;
  fontUpload.bytesWritten = 0;
  fontUpload.magic.fill(0);
  fontUpload.magicBytes = 0;
  fontUpload.bufferPos = 0;
  fontUpload.buffer.reset();
}

void CrossPointWebServer::abortFontUploadBatch(const char* errorMessage) {
  if (fontUpload.file.isOpen()) fontUpload.file.close();
  fontUpload.file = FsFile();
  if (!fontUpload.tempPath.empty() && Storage.exists(fontUpload.tempPath.c_str()) &&
      !Storage.remove(fontUpload.tempPath.c_str())) {
    LOG_ERR("WEB", "Failed to remove incomplete font upload: %s", fontUpload.tempPath.c_str());
  }

  if (fontUpload.transactionActive && !fontUpload.familyName.empty() &&
      !FontInstaller::rollbackFamilyInstall(fontUpload.familyName.c_str())) {
    LOG_ERR("WEB", "Font family rollback deferred until recovery: %s", fontUpload.familyName.c_str());
  }

  fontUpload.familyName.clear();
  fontUpload.finalPath.clear();
  fontUpload.tempPath.clear();
  fontUpload.errorMessage = errorMessage == nullptr ? "Font upload failed" : errorMessage;
  fontUpload.currentFilename.clear();
  fontUpload.lastCompletedFilename.clear();
  fontUpload.fingerprint = 0;
  fontUpload.sessionId = 0;
  fontUpload.currentIndex = 0;
  fontUpload.nextIndex = 0;
  fontUpload.totalFiles = 0;
  fontUpload.expectedFileSize = 0;
  fontUpload.lastCompletedFileSize = 0;
  fontUpload.transactionActive = false;
  fontUpload.requestValid = false;
  fontUpload.requestSucceeded = false;
  fontUpload.requestCommitted = false;
  fontUpload.cleanupPending = false;
  fontUpload.duplicateRequest = false;
  fontUpload.duplicateCommitted = false;
  fontUpload.magicChecked = false;
  fontUpload.bytesWritten = 0;
  fontUpload.magic.fill(0);
  fontUpload.magicBytes = 0;
  fontUpload.lastActivityAt = 0;
  fontUpload.lastCleanupAttemptAt = 0;
  fontUpload.bufferPos = 0;
  fontUpload.buffer.reset();
}

void CrossPointWebServer::handleFontUpload() {
  JsonDocument response;
  if (fontUpload.requestSucceeded) {
    response["ok"] = true;
    response["state"] = fontUpload.requestCommitted ? "committed" : "staged";
    if (fontUpload.requestCommitted) {
      sdFontSystem.markRegistryDirty();
      LOG_DBG("WEB", "Font family upload committed: %s", fontUpload.familyName.c_str());
    }
    String json;
    serializeJson(response, json);
    server->send(200, "application/json", json);
  } else {
    response["ok"] = false;
    response["error"] = fontUpload.errorMessage.empty() ? "Font upload failed" : fontUpload.errorMessage;
    String json;
    serializeJson(response, json);
    server->send(400, "application/json", json);
  }
}

void CrossPointWebServer::handleFontDelete() {
  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}
