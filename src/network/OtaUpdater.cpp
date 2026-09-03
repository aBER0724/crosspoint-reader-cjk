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

#include <cstring>

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/aBER0724/crosspoint-reader-cjk/releases/latest";
// The full /releases/latest body is ~9 KB today (notes + assets + author
// objects). Keep the accumulator cap at 12 KB: the guard trips BEFORE any
// append that would exceed capacity, so std::string never reallocates and
// never hits the doubling peak (old+new blocks live at once) that
// OOM-aborted the check on-device at ~21 KB free during body reception.
constexpr size_t RELEASE_JSON_MAX_BYTES = 12 * 1024;

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
  std::string releaseJson;
  // Reserve exactly once, on the FIRST body chunk: by then the TLS handshake
  // has completed and freed its transient allocations, so a single 12 KB
  // allocation is safe. Reserving before the handshake is what starved it
  // (field: -125 MEMORY_E at free=36656 with a 16 KB reserve in place).
  bool reserved = false;
  const bool fetchOk = HttpDownloader::fetchUrl(latestReleaseUrl, [&](const uint8_t* data, size_t len) {
    if (releaseJson.size() + len > RELEASE_JSON_MAX_BYTES) {
      LOG_ERR("OTA", "Release JSON exceeds %u bytes, aborting", static_cast<unsigned>(RELEASE_JSON_MAX_BYTES));
      return false;
    }
    if (!reserved) {
      releaseJson.reserve(RELEASE_JSON_MAX_BYTES);
      reserved = true;
    }
    releaseJson.append(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!fetchOk) {
    LOG_ERR("OTA", "Release metadata fetch failed");
    return HTTP_ERROR;
  }
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
