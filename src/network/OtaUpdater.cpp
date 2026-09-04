#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <HalStorage.h>
#include <Preferences.h>

#include <cstring>

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/aBER0724/crosspoint-reader-cjk/releases/latest";
// The full /releases/latest body is ~9 KB today (notes + assets + author
// objects). Staged on SD during the transfer with this as the upper bound;
// see checkForUpdate() for why it must never be held in RAM mid-transfer.
constexpr size_t RELEASE_JSON_MAX_BYTES = 12 * 1024;
constexpr char OTA_RELEASE_JSON_PATH[] = "/ota_release.tmp";

bool isTraditionalChineseBuild() { return strstr(CROSSPOINT_VERSION, "-tc") != nullptr; }

bool parseSemver3(const char* version, int* major, int* minor, int* patch) {
  if (!version || !major || !minor || !patch) {
    return false;
  }
  const char* p = version;
  if (*p == 'v' || *p == 'V') {
    p++;
  }
  return sscanf(p, "%d.%d.%d", major, minor, patch) == 3;
}

bool isFirmwareAssetName(const char* name) {
  if (!name) {
    return false;
  }
  const size_t len = strlen(name);
  if (len < 13) {  // "firmware-x.bin"
    return false;
  }
  return strncmp(name, "firmware", 8) == 0 && strcmp(name + len - 4, ".bin") == 0;
}

bool pickAssetByName(const JsonArrayConst& assets, const char* targetName, std::string* outUrl, size_t* outSize) {
  for (const JsonVariantConst asset : assets) {
    const char* name = asset["name"] | "";
    if (strcmp(name, targetName) == 0) {
      *outUrl = asset["browser_download_url"] | "";
      *outSize = asset["size"] | static_cast<size_t>(0);
      return !outUrl->empty() && *outSize > 0;
    }
  }
  return false;
}

bool pickAnyFirmwareAsset(const JsonArrayConst& assets, std::string* outUrl, size_t* outSize) {
  for (const JsonVariantConst asset : assets) {
    const char* name = asset["name"] | "";
    if (!isFirmwareAssetName(name)) {
      continue;
    }
    *outUrl = asset["browser_download_url"] | "";
    *outSize = asset["size"] | static_cast<size_t>(0);
    if (!outUrl->empty() && *outSize > 0) {
      return true;
    }
  }
  return false;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  JsonDocument filter;
  JsonDocument doc;
  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  // Fetch the release metadata through the same wolfSSL transport the firmware
  // download itself uses. The previous raw esp_http_client path ran mbedTLS,
  // whose ssl_setup needs ~35 KB of contiguous heap right after WiFi startup;
  // on X4 that allocation failed even with tens of KB free (field crash:
  // mbedtls_ssl_setup returned -0x7F00 at free=35732/max=29684). wolfSSL needs
  // the documented ~21.5 KB contiguous arena and tolerates the post-WiFi heap.
  //
  // Stage the response on the SD card (font-manifest pattern) instead of
  // accumulating it in RAM: at check time the heap still carries the WiFi
  // selector's scan leftovers and the live TLS session, and every large
  // std::string allocation in that window has aborted on device (16 KB
  // reserve before the handshake -> -125 MEMORY_E stall; 12 KB reserve at the
  // first body chunk -> bad_alloc -> abort, because the contiguous max block
  // there is under 12 KB even when free heap is ~36 KB). downloadToFile keeps
  // the transfer allocation-free (bounded by RELEASE_JSON_MAX_BYTES) and we
  // parse from the staged file only after the TLS client is destroyed.
  // maxBytes stays 0 on purpose: in downloadToFile a nonzero maxBytes on a
  // TLS 1.2 host switches runGetWolf into its ranged-GET mode (font-segment
  // semantics), and the GitHub releases API answers 200 without a
  // Content-Range header, which that path rejects (field: "Invalid
  // Content-Range for requested bytes 0-12287"). The body is bounded instead
  // by the explicit size check on the staged file below.
  const HttpDownloader::DownloadError dlError =
      HttpDownloader::downloadToFile(latestReleaseUrl, OTA_RELEASE_JSON_PATH, nullptr, nullptr, "", "", nullptr, 0);
  if (dlError != HttpDownloader::DownloadError::OK) {
    LOG_ERR("OTA", "Release metadata fetch failed: %d", static_cast<int>(dlError));
    return HTTP_ERROR;
  }
  std::string releaseJson;
  {
    HalFile file;
    if (!Storage.openFileForRead("OTA", OTA_RELEASE_JSON_PATH, file)) {
      LOG_ERR("OTA", "Staged release JSON missing");
      Storage.remove(OTA_RELEASE_JSON_PATH);
      return JSON_PARSE_ERROR;
    }
    const size_t jsonSize = file.size();
    if (jsonSize == 0 || jsonSize > RELEASE_JSON_MAX_BYTES) {
      LOG_ERR("OTA", "Staged release JSON has invalid size %u", static_cast<unsigned>(jsonSize));
      file.close();
      Storage.remove(OTA_RELEASE_JSON_PATH);
      return JSON_PARSE_ERROR;
    }
    // Belt and suspenders: the transfer is done and the TLS arena is freed,
    // so the JSON should fit easily; still, fail cleanly rather than letting
    // the reserve below abort() if the heap is unexpectedly squeezed.
    if (ESP.getMaxAllocHeap() < static_cast<int>(jsonSize + 4096)) {
      LOG_ERR("OTA", "Heap too low to parse release JSON (%u bytes, max=%d)", static_cast<unsigned>(jsonSize),
              ESP.getMaxAllocHeap());
      file.close();
      Storage.remove(OTA_RELEASE_JSON_PATH);
      return JSON_PARSE_ERROR;
    }
    releaseJson.reserve(jsonSize + 1);
    uint8_t chunk[512];
    int n;
    while ((n = file.read(chunk, sizeof(chunk))) > 0) {
      releaseJson.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
    }
    file.close();
  }
  Storage.remove(OTA_RELEASE_JSON_PATH);
  LOG_DBG("OTA", "Release JSON: %u bytes, free=%d max=%d", static_cast<unsigned>(releaseJson.size()), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;
  const DeserializationError error = deserializeJson(doc, releaseJson, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("OTA", "JSON parse failed: %s", error.c_str());
    return JSON_PARSE_ERROR;
  }

  if (!doc["tag_name"].is<std::string>()) {
    LOG_ERR("OTA", "No tag_name found");
    return JSON_PARSE_ERROR;
  }

  if (!doc["assets"].is<JsonArray>()) {
    LOG_ERR("OTA", "No assets found");
    return JSON_PARSE_ERROR;
  }

  latestVersion = doc["tag_name"].as<std::string>();

  const JsonArrayConst assets = doc["assets"].as<JsonArrayConst>();
  const char* preferredAssetName = isTraditionalChineseBuild() ? "firmware-tc.bin" : "firmware-sc.bin";

  if (!pickAssetByName(assets, preferredAssetName, &otaUrl, &otaSize) &&
      !pickAssetByName(assets, "firmware.bin", &otaUrl, &otaSize) && !pickAnyFirmwareAsset(assets, &otaUrl, &otaSize)) {
    LOG_ERR("OTA", "No firmware asset found (preferred: %s)", preferredAssetName);
    return NO_UPDATE;
  }

  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: %s, asset=%s, size=%u", latestVersion.c_str(), otaUrl.c_str(),
          static_cast<unsigned int>(otaSize));
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  if (!parseSemver3(latestVersion.c_str(), &latestMajor, &latestMinor, &latestPatch) ||
      !parseSemver3(currentVersion, &currentMajor, &currentMinor, &currentPatch)) {
    LOG_ERR("OTA", "Version parse failed: current=%s, latest=%s", currentVersion, latestVersion.c_str());
    return false;
  }

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this
  // package can't negotiate TLS 1.3 (see SecureClient.h). Drive the OTA partition
  // ourselves and stream the firmware through HttpDownloader, which runs over
  // wolfSSL when FREEINK_NET_WOLFSSL is set, reusing its redirect handling for the
  // GitHub -> CDN hop.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }
  LOG_DBG("OTA", "OTA begin OK: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  recordInstallProgressForDiagnostics(2, 0);
  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  const bool fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, size_t len) {
    if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
      flashOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        // Persist coarse progress so an interrupted install reports how far
        // it got. NVS wear is bounded: one write per whole percent bucket.
        if (pct % 10 == 0) {
          recordInstallProgressForDiagnostics(2, processedSize);
        }
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}

// --- Install failure diagnostics -------------------------------------------
// The install phase runs with the USB console unavailable for minutes at a
// time (and the failure path must not depend on serial output), so persist
// the essential state in NVS and print it once at the next boot.

namespace {
// Stage values written to NVS as the install flow progresses, so an
// interrupted run reports how far it got at the next boot.
constexpr uint8_t DIAG_STAGE_CHECK_OK = 1;   // update found, install about to start
constexpr uint8_t DIAG_STAGE_OTA_BEGIN = 2;  // esp_ota_begin succeeded, download running
constexpr uint8_t DIAG_STAGE_FAILED = 3;     // installUpdate returned an error

void writeDiagRecord(const uint8_t stage, const OtaUpdater::OtaUpdaterError error, const size_t processed,
                     const size_t total) {
  Preferences prefs;
  if (!prefs.begin("ota-diag", false)) {
    LOG_ERR("OTA", "Diag record: NVS open failed");
    return;
  }
  prefs.putUChar("stage", stage);
  prefs.putUChar("err", static_cast<uint8_t>(error));
  prefs.putULong("proc", processed);
  prefs.putULong("tot", total);
  prefs.putBool("valid", true);
  prefs.end();
}
}  // namespace

void OtaUpdater::recordInstallProgressForDiagnostics(const uint8_t stage, const size_t processed) {
  writeDiagRecord(stage, OK, processed, 0);
}

void OtaUpdater::recordInstallFailureForDiagnostics(const OtaUpdaterError error, const size_t processed,
                                                    const size_t total) {
  writeDiagRecord(DIAG_STAGE_FAILED, error, processed, total);
}

void OtaUpdater::reportAndClearLastInstallFailure() {
  Preferences prefs;
  if (!prefs.begin("ota-diag", true)) {
    return;
  }
  if (!prefs.getBool("valid", false)) {
    prefs.end();
    return;
  }
  const uint8_t stage = prefs.getUChar("stage", 0);
  const auto error = static_cast<OtaUpdaterError>(prefs.getUChar("err", 0));
  const size_t processed = prefs.getULong("proc", 0);
  const size_t total = prefs.getULong("tot", 0);
  prefs.clear();
  prefs.end();
  LOG_ERR("OTA", "Last OTA run: stage=%u err=%d processed=%u/%u free=%d max=%d", stage, static_cast<int>(error),
          static_cast<unsigned>(processed), static_cast<unsigned>(total), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void OtaUpdater::requestBootInstall() {
  Preferences prefs;
  if (!prefs.begin("ota-diag", false)) {
    LOG_ERR("OTA", "Boot install flag: NVS open failed");
    return;
  }
  prefs.putBool("bootInstall", true);
  prefs.end();
}

bool OtaUpdater::consumeBootInstallRequest() {
  Preferences prefs;
  if (!prefs.begin("ota-diag", false)) {
    return false;
  }
  const bool requested = prefs.getBool("bootInstall", false);
  if (requested) {
    // Consume before doing anything else so a crash during the install flow
    // can never turn into a boot loop.
    prefs.putBool("bootInstall", false);
  }
  prefs.end();
  return requested;
}

void OtaUpdater::clearBootInstallRequest() {
  Preferences prefs;
  if (!prefs.begin("ota-diag", false)) {
    return;
  }
  prefs.putBool("bootInstall", false);
  prefs.end();
}
