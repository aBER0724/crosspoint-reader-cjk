#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include "FontRepositoryStore.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/FontRepositoryListActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/FontRepositoryUtil.h"
#include "util/TaskWatchdog.h"

namespace {
constexpr size_t MAX_MANIFEST_BYTES = 128 * 1024;
constexpr size_t MAX_MANIFEST_FAMILIES = 48;
constexpr size_t MAX_FILES_PER_FAMILY = 32;
constexpr uint64_t STORAGE_RESERVE_BYTES = 8ULL * 1024ULL * 1024ULL;
constexpr const char* PREVIEW_TMP_PATH = "/.font_preview.cpfont";
constexpr const char* PREVIEW_NEXT_PATH = "/.font_preview_next.cpfont";
constexpr const char* PREVIEW_BACKUP_PATH = "/.font_preview_backup.cpfont";
constexpr uint8_t DEFAULT_PREVIEW_POINT_SIZE = 14;
constexpr int UI_PACKAGE_FILE_INDEX = -2;

constexpr const char* PREVIEW_SAMPLE_LINES[] = {
    "\xe9\x98\x85\xe8\xaf\xbb\xe9\xa2\x84\xe8\xa7\x88  "
    "\xe6\xb1\x89\xe5\xad\x97\xe6\x98\x8e\xe6\x9c\x9d\xe9\xbb\x91\xe4\xbd\x93\xe6\xa5\xb7\xe4\xbd\x93",
    "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87  "
    "\xe9\x96\xb1\xe8\xae\x80\xe9\xa0\x90\xe8\xa6\xbd\xe5\xad\x97\xe9\xab\x94",
    "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88\xe3\x81\x8a  "
    "\xe3\x82\xa2\xe3\x82\xa4\xe3\x82\xa6\xe3\x82\xa8\xe3\x82\xaa",
    "The quick brown fox 0123456789",
};
constexpr size_t MAX_BASE_URL_LENGTH = 256;
constexpr size_t MAX_DESCRIPTION_LENGTH = 160;
constexpr size_t MAX_FONT_FILE_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t SHA256_BYTES = 32;

bool parseSha256(const char* text, std::array<uint8_t, SHA256_BYTES>& outHash) {
  if (!text || strlen(text) != SHA256_BYTES * 2) return false;

  const auto hexNibble = [](const char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < outHash.size(); ++i) {
    const int high = hexNibble(text[i * 2]);
    const int low = hexNibble(text[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    outHash[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

uint32_t fingerprintBytes(const void* data, const size_t size, uint32_t fingerprint = 2166136261U) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    fingerprint ^= bytes[i];
    fingerprint *= 16777619U;
  }
  return fingerprint;
}

bool isValidBaseUrl(const std::string& url) {
  const bool isHttps = url.compare(0, 8, "https://") == 0;
  const bool isHttp = url.compare(0, 7, "http://") == 0;
  // HTTP is allowed for LAN-local CDN testing; per-file SHA-256 validation
  // already guarantees integrity regardless of transport security.
  if ((!isHttps && !isHttp) || url.empty() || url.size() > MAX_BASE_URL_LENGTH || url.back() != '/') return false;
  if (url.find_first_of(" \t\r\n\\?#") != std::string::npos) return false;

  const size_t hostStart = isHttps ? 8 : 7;
  const size_t pathStart = url.find('/', hostStart);
  return pathStart != std::string::npos && pathStart > hostStart && url.find("..", pathStart) == std::string::npos;
}

constexpr size_t MAX_MANIFEST_FAMILY_JSON_BYTES = 4 * 1024;
constexpr size_t MIN_NETWORK_FREE_HEAP_BYTES = 32 * 1024;
constexpr size_t MIN_NETWORK_MAX_ALLOC_BYTES = 21492;

bool seekToManifestFamilies(HalFile& file) {
  constexpr char KEY[] = "\"families\"";
  size_t matched = 0;
  bool inString = false;
  bool escaped = false;
  int depth = 0;

  while (file.available()) {
    const int raw = file.read();
    if (raw < 0) return false;
    const char ch = static_cast<char>(raw);

    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        inString = false;
      }
    } else if (ch == '"') {
      inString = true;
    } else if (ch == '{' || ch == '[') {
      ++depth;
    } else if (ch == '}' || ch == ']') {
      --depth;
    }

    if (depth == 1 && ch == KEY[matched]) {
      if (++matched == sizeof(KEY) - 1) {
        while (file.available()) {
          const int separator = file.read();
          if (separator < 0) return false;
          if (separator == ':') break;
          if (!isspace(separator)) return false;
        }
        while (file.available()) {
          const int arrayStart = file.read();
          if (arrayStart < 0) return false;
          if (arrayStart == '[') return true;
          if (!isspace(arrayStart)) return false;
        }
        return false;
      }
    } else {
      matched = ch == KEY[0] ? 1 : 0;
    }
  }
  return false;
}

bool parseNextManifestFamily(HalFile& file, std::string& familyJson, bool& reachedEnd) {
  familyJson.clear();
  reachedEnd = false;

  int raw = -1;
  while (file.available()) {
    raw = file.read();
    if (raw < 0) return false;
    if (raw == ']') {
      reachedEnd = true;
      return true;
    }
    if (raw == '{') break;
    if (raw != ',' && !isspace(raw)) return false;
  }
  if (raw != '{') return false;

  familyJson.reserve(2048);
  familyJson.push_back('{');
  int depth = 1;
  bool inString = false;
  bool escaped = false;
  while (depth > 0 && file.available()) {
    raw = file.read();
    if (raw < 0) return false;
    const char ch = static_cast<char>(raw);
    if (familyJson.size() >= MAX_MANIFEST_FAMILY_JSON_BYTES) return false;
    familyJson.push_back(ch);

    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        inString = false;
      }
    } else if (ch == '"') {
      inString = true;
    } else if (ch == '{' || ch == '[') {
      ++depth;
    } else if (ch == '}' || ch == ']') {
      --depth;
    }
  }
  return depth == 0;
}
}  // namespace

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  removePreviewTemporaryFiles();

  // The independent UI font normally keeps a large contiguous glyph cache.
  // Release it before the WiFi stack allocates its NVS and driver state; doing
  // this after WiFi.mode() is too late on X4 and can make WiFi initialization
  // fail with a null internal handle.
  {
    RenderLock lock(*this);
    LOG_DBG("FONT", "Heap before WiFi cache release: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    FontManager::getInstance().releaseGlyphCaches();
    sdFontSystem.releaseResidentFonts(renderer);
    if (auto* cache = renderer.getFontCacheManager()) {
      cache->clearCache();
    }
    LOG_DBG("FONT", "Heap before WiFi startup: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  if (!WiFi.mode(WIFI_STA)) {
    LOG_ERR("FONT", "Failed to initialize WiFi for font manager");
    errorMessage_ = tr(STR_WIFI_CONN_FAILED);
    state_ = ERROR;
    requestUpdate();
    return;
  }
  wifiStarted_ = true;
  // Wi-Fi station initialization needs the resident font allocations out of
  // the way, but scanning and connection screens do not need the TLS arena.
  // Restore the configured UI face now so the selector matches the rest of the
  // UI. beginNetworkTransfer() releases it again before manifest/font TLS.
  {
    RenderLock lock(*this);
    sdFontSystem.ensureUiLoaded(renderer);
  }
  LOG_DBG("FONT", "Configured UI fonts restored for WiFi selector: free=%d max=%d", ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();
  // ActivityManager invokes onExit() while holding the rendering mutex.
  closePreview();

  if (wifiStarted_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(30);
  }

  wifiStarted_ = false;
  // ActivityManager runs the parent result handler after onExit(). Keep the
  // resident set dirty here so that handler can rebuild its allocation-heavy
  // settings lists before restoring SD fonts and their coverage indexes.
  sdFontSystem.markResidentFontsDirty();
  FontManager::getInstance().releaseGlyphCaches();
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  reconnectSsid_ = WiFi.SSID().c_str();
  reconnectPassword_ = WiFi.psk().c_str();
  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  // The selector uses the configured SD UI face. Release it now, before the
  // manifest heap guard and TLS handshake; fetchManifestToFile() will call
  // beginNetworkTransfer() again harmlessly for each retry.
  beginNetworkTransfer();

  // Fail in-place if there is not enough contiguous memory for TLS. Rebooting
  // straight back into this activity restarts WiFi and can trap the user in a
  // Fonts -> WiFi -> reboot loop when the heap remains below the threshold.
  if (!heapSufficientForNetworkTransfer()) {
    LOG_DBG("FONT", "Heap too fragmented for manifest fetch: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    errorMessage_ = tr(STR_FONT_LIST_FETCH_FAILED);
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }
  if (!fetchAndParseManifests()) {
    if (cancelRequested_) {
      finish();
      return;
    }
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  // Tear down the TLS/WiFi allocator state before the already parsed catalog is
  // rendered. Resetting all retained catalog/UI caches is intentional here: with
  // 44 CJK families resident there is only ~3.5KB contiguous heap left, and
  // otherwise FAMILY_LIST's first item/detail render keeps hitting the low-memory
  // restart path. Font file transfers restart STA mode on demand.
  if (wifiStarted_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(30);
    wifiStarted_ = false;
  }
  FontManager::getInstance().releaseGlyphCaches();
  if (auto* cache = renderer.getFontCacheManager()) {
    cache->clearCache();
  }
  LOG_DBG("FONT", "Heap after WiFi/cache cleanup before family list: free=%d max=%d", ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  {
    RenderLock lock(*this);
    sdFontSystem.ensureLoaded(renderer);
  }
  LOG_DBG("FONT", "Configured UI fonts restored for family list: free=%d max=%d", ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
  }
  LOG_DBG("FONT", "Manifest complete: families=%u free=%u max=%u firstFamily='%s'",
          static_cast<unsigned>(families_.size()), ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          families_.empty() ? "" : families_[0].name.c_str());
}

// --- Manifest fetching ---
bool FontDownloadActivity::parsePointSize(const char* filename, const char* familyName, uint8_t& pointSize) {
  if (!filename || !familyName) return false;

  const std::string name(filename);
  const std::string prefix = std::string(familyName) + "_";
  static constexpr const char* EXTENSION = ".cpfont";
  const size_t extensionLength = strlen(EXTENSION);
  if (name.compare(0, prefix.size(), prefix) != 0 || name.size() <= prefix.size() + extensionLength ||
      name.compare(name.size() - extensionLength, extensionLength, EXTENSION) != 0) {
    return false;
  }

  const std::string pointSizeText = name.substr(prefix.size(), name.size() - prefix.size() - extensionLength);
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(pointSizeText.c_str(), &end, 10);
  if (!end || *end != '\0' || parsed == 0 || parsed > UINT8_MAX) return false;

  pointSize = static_cast<uint8_t>(parsed);
  return true;
}

int FontDownloadActivity::defaultPreviewFileIndex(const ManifestFamily& family) const {
  int bestIndex = 0;
  int bestDistance = 256;
  for (size_t i = 0; i < family.files.size(); ++i) {
    const int distance = abs(static_cast<int>(family.files[i].pointSize) - DEFAULT_PREVIEW_POINT_SIZE);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = static_cast<int>(i);
    }
  }
  return bestIndex;
}

bool FontDownloadActivity::fetchAndParseManifests() {
  cancelRequested_ = false;

#ifdef DEVICE_TEST
  if (Storage.exists("/fonts_test_manifest.json")) {
    std::vector<ManifestFamily> parsedFamilies;
    std::string parsedBaseUrl;
    std::string parsedUpdatedAt;
    if (!parseManifestFile("/fonts_test_manifest.json", parsedFamilies, parsedBaseUrl, parsedUpdatedAt, false)) {
      return false;
    }
    const auto sharedBaseUrl =
        std::shared_ptr<const std::string>(new (std::nothrow) std::string(std::move(parsedBaseUrl)));
    if (!sharedBaseUrl) {
      errorMessage_ = tr(STR_FONT_LIST_READ_FAILED);
      return false;
    }
    for (auto& family : parsedFamilies) {
      for (auto& file : family.files) file.baseUrl = sharedBaseUrl;
      refreshFamilyState(family);
    }
    manifestFiles_ = {"/fonts_test_manifest.json"};
    families_ = std::move(parsedFamilies);
    catalogUpdatedAt_ = parsedUpdatedAt;
    partialManifestFailure_ = false;
    return !families_.empty();
  }
#endif

  // Default repository first (compile-time URL), then user-configured
  // repositories in order. Earlier repositories win on dedupe, so a fork of
  // the default catalog contributes only its unique families / point sizes.
  std::vector<std::string> urls;
  urls.push_back(FONT_MANIFEST_URL);
  for (const auto& repo : FONT_REPO_STORE.getRepositories()) {
    urls.push_back(assembleManifestUrl(repo));
  }

  std::vector<ManifestFamily> mergedFamilies;
  bool anySuccess = false;
  bool someFailed = false;

  fontInstaller_.refreshRegistry();

  // Download every repository manifest to SD first (kept for later restore),
  // then parse + merge. Splitting the passes keeps the TLS/memory-heavy
  // download phase separate from the JSON parsing phase.
  manifestFiles_.clear();
  std::vector<std::string> downloadedPaths;
  for (size_t i = 0; i < urls.size(); ++i) {
    const std::string path = "/fonts_manifest_" + std::to_string(i) + ".tmp";
    if (!downloadManifestToFile(urls[i], path.c_str())) {
      if (cancelRequested_) return false;
      if (i == 0) {
        // The default repository is authoritative; a failure there is fatal.
        return false;
      }
      someFailed = true;
      continue;
    }
    downloadedPaths.push_back(path);
    anySuccess = true;
  }

  if (!anySuccess) {
    errorMessage_ = tr(STR_FONT_LIST_FETCH_FAILED);
    return false;
  }
  manifestFiles_ = std::move(downloadedPaths);

  if (wifiStarted_ && WiFi.getMode() != WIFI_MODE_NULL) {
    LOG_DBG("FONT", "Heap before WiFi shutdown for manifest parse: free=%d max=%d", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(30);
    wifiStarted_ = false;
    FontManager::getInstance().releaseGlyphCaches();
    if (auto* cache = renderer.getFontCacheManager()) {
      cache->clearCache();
    }
    LOG_DBG("FONT", "Heap after WiFi shutdown for manifest parse: free=%d max=%d", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }

  mergedFamilies.reserve(MAX_MANIFEST_FAMILIES);
  for (size_t i = 0; i < manifestFiles_.size(); ++i) {
    std::vector<ManifestFamily> parsedFamilies;
    std::string parsedBaseUrl;
    std::string parsedUpdatedAt;
    if (!parseManifestFile(manifestFiles_[i].c_str(), parsedFamilies, parsedBaseUrl, parsedUpdatedAt, true)) {
      errorMessage_ = tr(STR_FONT_LIST_READ_FAILED);
      return false;
    }
    const auto sharedBaseUrl =
        std::shared_ptr<const std::string>(new (std::nothrow) std::string(std::move(parsedBaseUrl)));
    if (!sharedBaseUrl) {
      errorMessage_ = tr(STR_FONT_LIST_READ_FAILED);
      return false;
    }
    for (auto& family : parsedFamilies) {
      for (auto& file : family.files) file.baseUrl = sharedBaseUrl;
    }
    mergeManifestFamilies(mergedFamilies, std::move(parsedFamilies));
    if (!parsedUpdatedAt.empty() && parsedUpdatedAt > catalogUpdatedAt_) {
      catalogUpdatedAt_ = parsedUpdatedAt;
    }
  }

  for (auto& family : mergedFamilies) refreshFamilyState(family);
  families_ = std::move(mergedFamilies);
  partialManifestFailure_ = someFailed;
  LOG_DBG("FONT", "Manifest loaded: %zu families (%zu repos, partial=%d)", families_.size(), urls.size(),
          static_cast<int>(someFailed));
  return true;
}

bool FontDownloadActivity::restoreManifestData() {
  // Re-parse the kept per-repo manifest files and rebuild families_. Mirror of
  // the parse+merge pass in fetchAndParseManifests: same files, same order, so
  // family indexes are preserved across a transfer.
  std::vector<ManifestFamily> mergedFamilies;
  mergedFamilies.reserve(MAX_MANIFEST_FAMILIES);
  for (const auto& path : manifestFiles_) {
    std::vector<ManifestFamily> parsedFamilies;
    std::string parsedBaseUrl;
    std::string parsedUpdatedAt;
    if (!parseManifestFile(path.c_str(), parsedFamilies, parsedBaseUrl, parsedUpdatedAt, false)) {
      LOG_ERR("FONT", "Failed to restore manifest from %s", path.c_str());
      families_.clear();
      return false;
    }
    const auto sharedBaseUrl =
        std::shared_ptr<const std::string>(new (std::nothrow) std::string(std::move(parsedBaseUrl)));
    if (!sharedBaseUrl) {
      LOG_ERR("FONT", "Unable to allocate restored manifest base URL");
      families_.clear();
      return false;
    }
    for (auto& family : parsedFamilies) {
      for (auto& file : family.files) file.baseUrl = sharedBaseUrl;
    }
    mergeManifestFamilies(mergedFamilies, std::move(parsedFamilies));
    if (!parsedUpdatedAt.empty() && parsedUpdatedAt > catalogUpdatedAt_) {
      catalogUpdatedAt_ = parsedUpdatedAt;
    }
  }
  if (mergedFamilies.empty()) {
    LOG_ERR("FONT", "Manifest restore produced no families");
    families_.clear();
    return false;
  }
  for (auto& family : mergedFamilies) refreshFamilyState(family);
  LOG_DBG("FONT", "Restored %zu families from storage", mergedFamilies.size());
  families_ = std::move(mergedFamilies);
  return true;
}

bool FontDownloadActivity::downloadManifestToFile(const std::string& url, const char* path) {
  // Download the raw manifest to a file on SD card to avoid holding both
  // TLS buffers and the full JSON string in RAM simultaneously. The file is
  // kept so families_ can be released during large font-file transfers and
  // restored (re-parsed) afterwards without re-downloading.
  constexpr int MAX_DOWNLOAD_ATTEMPTS = 8;
  for (int attempt = 1; attempt <= MAX_DOWNLOAD_ATTEMPTS; ++attempt) {
    if (Storage.exists(path) && !Storage.remove(path)) {
      LOG_ERR("FONT", "Failed to remove stale font manifest");
      errorMessage_ = tr(STR_FONT_LIST_READ_FAILED);
      return false;
    }

    beginNetworkTransfer();
    if (!ensureWifiConnectedForTransfer()) {
      endNetworkTransfer();
      Storage.remove(path);
      if (cancelRequested_) return false;
      continue;
    }
    const auto result = HttpDownloader::downloadToFile(
        url.c_str(), path, [this](size_t, size_t) { pollDownloadCancellation(); }, &cancelRequested_, "", "",
        [this] { return pollDownloadCancellation(); }, MAX_MANIFEST_BYTES);
    endNetworkTransfer();
    if (result == HttpDownloader::ABORTED || cancelRequested_) {
      Storage.remove(path);
      return false;
    }
    if (result == HttpDownloader::OK) return true;

    Storage.remove(path);
    if (attempt < MAX_DOWNLOAD_ATTEMPTS) {
      LOG_ERR("FONT", "Manifest download attempt %d/%d failed; retrying", attempt, MAX_DOWNLOAD_ATTEMPTS);
      delay(250);
      resetTaskWatchdogIfSubscribed();
    }
  }

  LOG_ERR("FONT", "Failed to fetch manifest from %s after retries", url.c_str());
  errorMessage_ = tr(STR_FONT_LIST_FETCH_FAILED);
  return false;
}

bool FontDownloadActivity::parseManifestFile(const char* path, std::vector<ManifestFamily>& outFamilies,
                                             std::string& outBaseUrl, std::string& outUpdatedAt,
                                             const bool /*allowSelfHealRestart*/) {
  // HTTP client is now closed and TLS buffers are freed. Parse JSON from file.
  HalFile manifestFile;
  if (!Storage.openFileForRead("FONT", path, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    errorMessage_ = tr(STR_FONT_LIST_READ_FAILED);
    return false;
  }

  const size_t manifestSize = manifestFile.fileSize();
  if (manifestSize == 0 || manifestSize > MAX_MANIFEST_BYTES) {
    LOG_ERR("FONT", "Manifest size is invalid: %zu", manifestSize);
    manifestFile.close();
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }

  // Parse only the small top-level metadata first. The families array is then
  // scanned from SD and each family is deserialized independently, keeping the
  // peak ArduinoJson document below the largest single family (~1.2 KB) instead
  // of materializing the complete 67 KB catalog.
  JsonDocument metadataFilter;
  metadataFilter["version"] = true;
  metadataFilter["baseUrl"] = true;
  metadataFilter["updatedAt"] = true;
  JsonDocument metadata;
  const DeserializationError metadataError =
      deserializeJson(metadata, manifestFile, DeserializationOption::Filter(metadataFilter));
  if (metadataError) {
    LOG_ERR("FONT", "Manifest metadata parse error: %s", metadataError.c_str());
    manifestFile.close();
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }

  const int version = metadata["version"] | 0;
  if (version != FONTS_MANIFEST_VERSION) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", version);
    manifestFile.close();
    errorMessage_ = tr(STR_FONT_MANIFEST_UNSUPPORTED);
    return false;
  }
  if (!metadata["baseUrl"].is<const char*>()) {
    manifestFile.close();
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }

  std::string parsedBaseUrl = metadata["baseUrl"].as<const char*>();
  if (!isValidBaseUrl(parsedBaseUrl)) {
    LOG_ERR("FONT", "Invalid manifest base URL");
    manifestFile.close();
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }

  outUpdatedAt.clear();
  if (!metadata["updatedAt"].isNull()) {
    if (!metadata["updatedAt"].is<const char*>()) {
      manifestFile.close();
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }
    outUpdatedAt = metadata["updatedAt"].as<const char*>();
  }
  metadata.clear();
  metadata.shrinkToFit();
  metadataFilter.clear();
  metadataFilter.shrinkToFit();

  if (!manifestFile.seek(0) || !seekToManifestFamilies(manifestFile)) {
    manifestFile.close();
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }

  std::vector<ManifestFamily> parsedFamilies;
  parsedFamilies.reserve(MAX_MANIFEST_FAMILIES);
  std::string familyJson;
  bool reachedEnd = false;
  while (!reachedEnd) {
    resetTaskWatchdogIfSubscribed();
    if (!parseNextManifestFamily(manifestFile, familyJson, reachedEnd)) {
      manifestFile.close();
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }
    if (reachedEnd) break;
    if (parsedFamilies.size() >= MAX_MANIFEST_FAMILIES) {
      LOG_ERR("FONT", "Too many font families");
      manifestFile.close();
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }

    JsonDocument familyDoc;
    const DeserializationError familyError = deserializeJson(familyDoc, familyJson);
    if (familyError || !familyDoc.is<JsonObject>()) {
      LOG_ERR("FONT", "Manifest family parse error: %s", familyError.c_str());
      manifestFile.close();
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }
    JsonObject fObj = familyDoc.as<JsonObject>();
    if (!fObj["name"].is<const char*>() || !fObj["files"].is<JsonArray>()) {
      manifestFile.close();
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }

    ManifestFamily family;
    family.name = fObj["name"].as<const char*>();
    if (!FontInstaller::isValidFamilyName(family.name.c_str())) {
      LOG_ERR("FONT", "Invalid family name in manifest");
      manifestFile.close();
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }
    for (const auto& existing : parsedFamilies) {
      if (existing.name == family.name) {
        LOG_ERR("FONT", "Duplicate family in manifest: %s", family.name.c_str());
        manifestFile.close();
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
    }

    if (!fObj["description"].isNull()) {
      if (!fObj["description"].is<const char*>()) {
        manifestFile.close();
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      family.description = fObj["description"].as<const char*>();
      if (family.description.size() > MAX_DESCRIPTION_LENGTH) {
        manifestFile.close();
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
    }

    JsonArray files = fObj["files"].as<JsonArray>();
    if (files.size() == 0 || files.size() > MAX_FILES_PER_FAMILY) {
      manifestFile.close();
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }
    family.files.reserve(files.size());

    for (JsonVariant fileValue : files) {
      if (!fileValue.is<JsonObject>()) {
        manifestFile.close();
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      JsonObject fileObj = fileValue.as<JsonObject>();
      if (!fileObj["name"].is<const char*>() || !fileObj["size"].is<size_t>() || !fileObj["sha256"].is<const char*>()) {
        manifestFile.close();
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }

      const char* fileName = fileObj["name"].as<const char*>();
      ManifestFile file;
      file.size = fileObj["size"].as<size_t>();
      if (!parseSha256(fileObj["sha256"].as<const char*>(), file.sha256)) {
        LOG_ERR("FONT", "Invalid SHA-256 in manifest: %s", fileName);
        manifestFile.close();
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      file.fingerprint = fingerprintBytes(fileName, strlen(fileName));
      file.fingerprint = fingerprintBytes(&file.size, sizeof(file.size), file.fingerprint);
      file.fingerprint = fingerprintBytes(file.sha256.data(), file.sha256.size(), file.fingerprint);
      if (!parsePointSize(fileName, family.name.c_str(), file.pointSize)) {
        LOG_ERR("FONT", "Font filename does not match family/size convention: %s", fileName);
        manifestFile.close();
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      if (!FontInstaller::isValidCpfontFilename(fileName) || file.size == 0 || file.size > MAX_FONT_FILE_BYTES ||
          file.size > std::numeric_limits<size_t>::max() - family.totalSize) {
        LOG_ERR("FONT", "Invalid file entry in manifest: %s", fileName);
        manifestFile.close();
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      for (const auto& existing : family.files) {
        if (existing.pointSize == file.pointSize) {
          LOG_ERR("FONT", "Duplicate point size in family %s: %u", family.name.c_str(), file.pointSize);
          manifestFile.close();
          errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
          return false;
        }
      }
      char path[160];
      if (!FontInstaller::buildFontPath(family.name.c_str(), fileName, path, sizeof(path))) {
        manifestFile.close();
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }

      family.totalSize += file.size;
      family.files.push_back(std::move(file));
    }
    std::sort(family.files.begin(), family.files.end(),
              [](const ManifestFile& a, const ManifestFile& b) { return a.pointSize < b.pointSize; });

    uint32_t fingerprint = 2166136261U;
    for (const auto& file : family.files) {
      const std::string fileName = manifestFileName(family, file);
      fingerprint = fingerprintBytes(fileName.data(), fileName.size(), fingerprint);
      fingerprint = fingerprintBytes(&file.size, sizeof(file.size), fingerprint);
      fingerprint = fingerprintBytes(file.sha256.data(), file.sha256.size(), fingerprint);
    }
    family.fingerprint = fingerprint;
    parsedFamilies.push_back(std::move(family));
  }

  manifestFile.close();
  if (parsedFamilies.empty()) {
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }
  outBaseUrl = std::move(parsedBaseUrl);
  outFamilies = std::move(parsedFamilies);
  return true;
}

void FontDownloadActivity::openFontRepositories() {
  // Capture the configured set before entering the management screen so we can
  // tell whether anything changed when the user returns.
  const auto previousRepos = FONT_REPO_STORE.getRepositories();
  startActivityForResult(std::make_unique<FontRepositoryListActivity>(renderer, mappedInput),
                         [this, previousRepos](const ActivityResult&) {
                           FONT_REPO_STORE.loadFromFile();
                           if (FONT_REPO_STORE.getRepositories() != previousRepos) {
                             // Re-fetch and re-merge so the catalog reflects the
                             // new repository set without a manual re-entry.
                             state_ = LOADING_MANIFEST;
                             requestUpdateAndWait();
                             if (!fetchAndParseManifests()) {
                               if (cancelRequested_) {
                                 finish();
                                 return;
                               }
                               state_ = ERROR;
                             } else {
                               state_ = FAMILY_LIST;
                               selectedIndex_ = 0;
                             }
                           }
                           requestUpdate();
                         });
}

bool FontDownloadActivity::pollDownloadCancellation() {
  mappedInput.update();
  if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    cancelRequested_ = true;
  }
  return cancelRequested_;
}

void FontDownloadActivity::beginNetworkTransfer(int activeFamilyIndex) {
  RenderLock lock(*this);
  lowMemoryDownload_ = true;
  lastProgressPercent_ = -1;

  LOG_DBG("FONT", "Heap before font cache release: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  FontManager::getInstance().releaseGlyphCaches();
  sdFontSystem.releaseResidentFonts(renderer);
  if (auto* cache = renderer.getFontCacheManager()) {
    cache->clearCache();
  }
  LOG_DBG("FONT", "Heap before network transfer: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // For font-file transfers (not the manifest fetch) release the parsed family
  // list: the caller keeps the active family as a local copy, and ~25-30KB of
  // dead-weight strings (URLs/sha256/descriptions for every other family) gives
  // the TLS handshake and 4.5MB+ stream the headroom it needs. Restored in
  // endNetworkTransfer from the kept manifest files.
  if (activeFamilyIndex >= 0 && activeFamilyIndex < static_cast<int>(families_.size()) && !manifestFiles_.empty() &&
      !manifestReleasedForTransfer_) {
    LOG_DBG("FONT", "Releasing manifest data before transfer: free=%d", ESP.getFreeHeap());
    std::vector<ManifestFamily>().swap(families_);
    manifestReleasedForTransfer_ = true;
    LOG_DBG("FONT", "Manifest data released: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
}

bool FontDownloadActivity::endNetworkTransfer() {
  if (manifestReleasedForTransfer_) {
    manifestReleasedForTransfer_ = false;
    // Release the WiFi/TLS allocator state before rebuilding the catalog.
    if (wifiStarted_ && WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(30);
      wifiStarted_ = false;
    }
    LOG_DBG("FONT", "Restoring manifest data after transfer: free=%d", ESP.getFreeHeap());
    if (!restoreManifestData()) {
      LOG_ERR("FONT", "Manifest restore failed after transfer");
      return false;
    }
    LOG_DBG("FONT", "Manifest restored: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  if (WiFi.getMode() == WIFI_MODE_NULL) {
    RenderLock lock(*this);
    sdFontSystem.ensureLoaded(renderer);
  }
  // Keep the low-memory progress renderer active until the entire install
  // transaction, including registry discovery, has completed.
  return true;
}

void FontDownloadActivity::updateDownloadProgress(const size_t downloaded, const size_t total) {
  fileProgress_ = downloaded;
  fileTotal_ = total;
  pollDownloadCancellation();

  if (total == 0) return;
  const int percent = static_cast<int>((static_cast<uint64_t>(downloaded) * 100U) / total);
  if (percent < 100 && percent / 5 == lastProgressPercent_ / 5) return;
  lastProgressPercent_ = percent;
  requestUpdate(true);
}

void FontDownloadActivity::refreshFamilyState(ManifestFamily& family) {
  const auto* installedFamily = sdFontSystem.registry().findFamily(family.name);
  family.installed = installedFamily != nullptr;
  bool allManifestFilesInstalled = family.installed;
  for (auto& file : family.files) {
    file.installed = installedFamily && installedFamily->hasSize(file.pointSize);
    if (!file.installed) allManifestFilesInstalled = false;
  }
  const bool hasReceipt = family.installed && FontInstaller::hasInstalledFamilyReceipt(family.name.c_str());
  family.partial = family.installed && (!hasReceipt || !allManifestFilesInstalled);
  family.hasUpdate = family.installed && !family.partial &&
                     !FontInstaller::installedFamilyMatches(family.name.c_str(), family.fingerprint);
}

// --- Download ---

bool FontDownloadActivity::ensureWifiConnectedForTransfer() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (!WiFi.mode(WIFI_STA)) return false;
  wifiStarted_ = true;
  if (!reconnectSsid_.empty()) {
    WiFi.begin(reconnectSsid_.c_str(), reconnectPassword_.c_str());
  } else {
    WiFi.begin();
  }
  const unsigned long deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(millis() - deadline) < 0) {
    if (pollDownloadCancellation()) return false;
    resetTaskWatchdogIfSubscribed();
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed && !families_[i].partial) continue;
    downloadFamily(static_cast<int>(i));
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (!families_[i].hasUpdate) continue;
    downloadFamily(static_cast<int>(i));
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const auto& f : families_) {
    if (!f.installed || f.partial) return true;
  }
  return false;
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const auto& f : families_) {
    if (f.hasUpdate) return true;
  }
  return false;
}

int FontDownloadActivity::specialRowCount() const {
  // Row 0 is always the "Font repositories" entry; Download All / Update All
  // follow it when shown.
  return 1 + (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool FontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 1; }

bool FontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 2 : 1);
}

int FontDownloadActivity::listItemCount() const { return static_cast<int>(families_.size()) + specialRowCount(); }

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed || f.partial) {
      if (f.totalSize > std::numeric_limits<size_t>::max() - total) return std::numeric_limits<size_t>::max();
      total += f.totalSize;
    }
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (f.hasUpdate) {
      if (f.totalSize > std::numeric_limits<size_t>::max() - total) return std::numeric_limits<size_t>::max();
      total += f.totalSize;
    }
  }
  return total;
}

void FontDownloadActivity::removePreviewTemporaryFiles() {
  if (Storage.exists(PREVIEW_TMP_PATH) && !Storage.remove(PREVIEW_TMP_PATH)) {
    LOG_ERR("FONT", "Failed to remove preview file");
  }
  if (Storage.exists(PREVIEW_NEXT_PATH) && !Storage.remove(PREVIEW_NEXT_PATH)) {
    LOG_ERR("FONT", "Failed to remove staged preview file");
  }
  if (Storage.exists(PREVIEW_BACKUP_PATH) && !Storage.remove(PREVIEW_BACKUP_PATH)) {
    LOG_ERR("FONT", "Failed to remove preview backup");
  }
}

bool FontDownloadActivity::computeFileSha256(const char* path, std::array<uint8_t, SHA256_BYTES>& outHash) {
  HalFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
  constexpr size_t BUF_SIZE = 4 * 1024;
  auto buf = makeUniqueNoThrow<uint8_t[]>(BUF_SIZE);
  if (!buf) {
    LOG_ERR("FONT", "Unable to allocate SHA-256 buffer");
    f.close();
    return false;
  }

  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  if (mbedtls_sha256_starts(&context, 0) != 0) {
    LOG_ERR("FONT", "Unable to initialize SHA-256");
    mbedtls_sha256_free(&context);
    f.close();
    return false;
  }

  const size_t fileSize = f.fileSize();
  size_t bytesRead = 0;
  while (bytesRead < fileSize) {
    if ((bytesRead & 0xFFFF) == 0) {
      yield();
      resetTaskWatchdogIfSubscribed();
      if (pollDownloadCancellation()) {
        mbedtls_sha256_free(&context);
        f.close();
        return false;
      }
    }
    const size_t remaining = fileSize - bytesRead;
    const int n = f.read(buf.get(), remaining < BUF_SIZE ? remaining : BUF_SIZE);
    if (n <= 0) {
      LOG_ERR("FONT", "Read failed during SHA-256 check: %s", path);
      mbedtls_sha256_free(&context);
      f.close();
      return false;
    }
    if (mbedtls_sha256_update(&context, buf.get(), static_cast<size_t>(n)) != 0) {
      LOG_ERR("FONT", "SHA-256 update failed: %s", path);
      mbedtls_sha256_free(&context);
      f.close();
      return false;
    }
    bytesRead += static_cast<size_t>(n);
  }
  const bool success = mbedtls_sha256_finish(&context, outHash.data()) == 0;
  mbedtls_sha256_free(&context);
  f.close();
  if (!success) LOG_ERR("FONT", "SHA-256 finalization failed: %s", path);
  return success;
}
void FontDownloadActivity::closePreview() {
  if (previewFontId_ != 0) {
    sdFontSystem.endPreview(renderer);
    previewFontId_ = 0;
  }
  activePreviewFamilyIndex_ = -1;
  activePreviewFileIndex_ = -1;
  removePreviewTemporaryFiles();
}

void FontDownloadActivity::returnToFamilyList() {
  RenderLock lock(*this);
  closePreview();
  state_ = FAMILY_LIST;
  errorAction_ = ErrorAction::None;
  // Rendering the 27-family list needs the UI glyph caches rebuilt plus row
  // buffers; right after a preview the heap is at its most fragmented (~3 KB
  // max alloc). Restart instead of aborting on an allocation failure.
  if (ESP.getFreeHeap() < 16 * 1024 || ESP.getMaxAllocHeap() < 8 * 1024) {
    LOG_DBG("FONT", "Heap too fragmented to draw family list after preview: free=%d max=%d; silent-restarting",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    silentRestartToFonts();
    return;
  }
}

void FontDownloadActivity::downloadPreview(int familyIndex, int fileIndex) {
  if (state_ == DOWNLOADING_PREVIEW || state_ == DOWNLOADING) return;
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return;
  // Work from a copy: beginNetworkTransfer(familyIndex) releases families_ for
  // the duration of the transfer, and this copy stays valid throughout.
  ManifestFamily family = families_[familyIndex];
  if (fileIndex < 0 || fileIndex >= static_cast<int>(family.files.size())) return;
  const auto& file = family.files[fileIndex];
  LOG_DBG("FONT", "DOWNLOAD_PREVIEW: family=%d name='%s' file=%d pt=%u free=%u max=%u", familyIndex,
          family.name.c_str(), fileIndex, static_cast<unsigned>(file.pointSize), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  const std::string fileName = manifestFileName(family, file);
  const bool hadPreview = previewFontId_ != 0;
  const int previousFamilyIndex = activePreviewFamilyIndex_;
  const int previousFileIndex = activePreviewFileIndex_;
  // Unload the on-screen preview BEFORE restoring the reader UI font or
  // rendering DOWNLOADING_PREVIEW. Preserve PREVIEW_TMP_PATH: after the new
  // file downloads, the activation transaction renames this current preview
  // to PREVIEW_BACKUP_PATH so it can roll back safely on failure.
  if (previewFontId_ != 0) {
    sdFontSystem.endPreview(renderer, false);
    previewFontId_ = 0;
  }
  activePreviewFamilyIndex_ = -1;
  activePreviewFileIndex_ = -1;
  // Safety net: rendering this screen must (re)load the reader UI font; a
  // heavily fragmented heap here previously aborted the device. The preview
  // glyphs are gone at this point, so this only fires
  // when the whole UI cache is starved; calibrate below the cold-boot
  // baseline (free ~14.5 KB / max ~5 KB).
  if (ESP.getFreeHeap() < 9 * 1024 || ESP.getMaxAllocHeap() < 3 * 1024) {
    LOG_DBG("FONT", "Heap too fragmented to show preview download screen: free=%d max=%d; silent-restarting",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    silentRestartToFonts();
    return;
  }
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING_PREVIEW;
    previewFamilyIndex_ = familyIndex;
    previewFileIndex_ = fileIndex;
    fileProgress_ = 0;
    fileTotal_ = file.size;
    cancelRequested_ = false;
    errorAction_ = ErrorAction::Preview;
  }
  requestUpdateAndWait();

  auto failPreview = [this, &fileName, hadPreview, previousFamilyIndex, previousFileIndex](const char* message,
                                                                                           bool cancelled) {
    if (Storage.exists(PREVIEW_NEXT_PATH)) Storage.remove(PREVIEW_NEXT_PATH);
    RenderLock lock(*this);
    if (cancelled) {
      if (hadPreview) {
        previewFamilyIndex_ = previousFamilyIndex;
        previewFileIndex_ = previousFileIndex;
        if (previousFamilyIndex >= 0 && previousFamilyIndex < static_cast<int>(families_.size()) &&
            previousFileIndex >= 0 &&
            previousFileIndex < static_cast<int>(families_[previousFamilyIndex].files.size()) &&
            Storage.exists(PREVIEW_TMP_PATH)) {
          previewFontId_ =
              sdFontSystem.beginPreview(renderer, PREVIEW_TMP_PATH, families_[previousFamilyIndex].name.c_str(),
                                        families_[previousFamilyIndex].files[previousFileIndex].pointSize);
          if (previewFontId_ != 0) {
            activePreviewFamilyIndex_ = previousFamilyIndex;
            activePreviewFileIndex_ = previousFileIndex;
          }
        }
        state_ = FONT_PREVIEW;
      } else {
        state_ = FAMILY_LIST;
      }
      errorAction_ = ErrorAction::None;
    } else {
      state_ = ERROR;
      errorMessage_ = std::string(message) + ": " + fileName;
    }
  };

  const uint64_t requiredBytes = static_cast<uint64_t>(file.size) + STORAGE_RESERVE_BYTES;
  if (Storage.freeBytes() < requiredBytes) {
    failPreview(tr(STR_FONT_INSUFFICIENT_STORAGE), false);
    requestUpdateAndWait();
    return;
  }

  if (Storage.exists(PREVIEW_NEXT_PATH) && !Storage.remove(PREVIEW_NEXT_PATH)) {
    failPreview(tr(STR_FONT_CLEANUP_FAILED), false);
    requestUpdateAndWait();
    return;
  }

  // The heap guard for the download itself is checked AFTER
  // beginNetworkTransfer below, once the glyph caches and families_ have been
  // released. Checking earlier would measure families_ still resident (~5 KB
  // max alloc) and false-trigger an endless silent-restart loop.
  beginNetworkTransfer(familyIndex);
  // Safety net: at this point the heap is the real TLS arena; a fragmented
  // heap here means the handshake would fail with wolfSSL -125, so restart
  // (a fresh boot defrags and retries from the family list).
  if (ESP.getFreeHeap() < 32 * 1024 || ESP.getMaxAllocHeap() < 21 * 1024) {
    LOG_DBG("FONT", "Heap too fragmented for preview transfer: free=%d max=%d; silent-restarting", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    silentRestartToFonts();
    return;
  }
  if (!ensureWifiConnectedForTransfer()) {
    endNetworkTransfer();
    failPreview(tr(STR_WIFI_CONN_FAILED), cancelRequested_);
    requestUpdateAndWait();
    return;
  }
  const auto result = HttpDownloader::downloadToFile(
      *file.baseUrl + fileName, PREVIEW_NEXT_PATH,
      [this](size_t downloaded, size_t total) { updateDownloadProgress(downloaded, total); }, &cancelRequested_, "", "",
      [this] { return pollDownloadCancellation(); }, file.size);
  if (!endNetworkTransfer()) {
    failPreview(tr(STR_FONT_LIST_READ_FAILED), false);
    requestUpdateAndWait();
    return;
  }
  if (result == HttpDownloader::ABORTED) {
    failPreview("", true);
    requestUpdateAndWait();
    return;
  }
  if (result != HttpDownloader::OK) {
    failPreview(tr(STR_DOWNLOAD_FAILED), false);
    requestUpdateAndWait();
    return;
  }
  if (pollDownloadCancellation()) {
    failPreview("", true);
    requestUpdateAndWait();
    return;
  }

  HalFile downloadedFile;
  if (!Storage.openFileForRead("FONT", PREVIEW_NEXT_PATH, downloadedFile)) {
    failPreview(tr(STR_DOWNLOAD_FAILED), false);
    requestUpdateAndWait();
    return;
  }
  const size_t actualSize = downloadedFile.fileSize();
  downloadedFile.close();
  if (actualSize != file.size) {
    failPreview(tr(STR_FONT_FILE_SIZE_MISMATCH), false);
    requestUpdateAndWait();
    return;
  }

  std::array<uint8_t, SHA256_BYTES> actualSha256{};
  if (!computeFileSha256(PREVIEW_NEXT_PATH, actualSha256)) {
    if (cancelRequested_) {
      failPreview("", true);
      requestUpdateAndWait();
      return;
    }
    failPreview(tr(STR_FONT_CHECKSUM_FAILED), false);
    requestUpdateAndWait();
    return;
  }
  if (actualSha256 != file.sha256) {
    failPreview(tr(STR_FONT_CHECKSUM_MISMATCH), false);
    requestUpdateAndWait();
    return;
  }
  if (pollDownloadCancellation()) {
    failPreview("", true);
    requestUpdateAndWait();
    return;
  }
  if (!fontInstaller_.validateCpfontFile(PREVIEW_NEXT_PATH)) {
    failPreview(tr(STR_FONT_INVALID_FILE), false);
    requestUpdateAndWait();
    return;
  }
  if (pollDownloadCancellation()) {
    failPreview("", true);
    requestUpdateAndWait();
    return;
  }

  {
    RenderLock lock(*this);
    bool canActivate = true;
    if (Storage.exists(PREVIEW_BACKUP_PATH) && !Storage.remove(PREVIEW_BACKUP_PATH)) {
      canActivate = false;
      errorMessage_ = std::string(tr(STR_FONT_CLEANUP_FAILED)) + ": " + fileName;
    }

    if (canActivate && hadPreview) {
      sdFontSystem.endPreview(renderer);
      previewFontId_ = 0;
      activePreviewFamilyIndex_ = -1;
      activePreviewFileIndex_ = -1;
      if (!Storage.rename(PREVIEW_TMP_PATH, PREVIEW_BACKUP_PATH)) {
        previewFontId_ =
            sdFontSystem.beginPreview(renderer, PREVIEW_TMP_PATH, families_[previousFamilyIndex].name.c_str(),
                                      families_[previousFamilyIndex].files[previousFileIndex].pointSize);
        if (previewFontId_ != 0) {
          activePreviewFamilyIndex_ = previousFamilyIndex;
          activePreviewFileIndex_ = previousFileIndex;
        }
        canActivate = false;
        errorMessage_ = std::string(tr(STR_FONT_REPLACEMENT_FAILED)) + ": " + fileName;
      }
    } else if (canActivate && Storage.exists(PREVIEW_TMP_PATH) && !Storage.remove(PREVIEW_TMP_PATH)) {
      canActivate = false;
      errorMessage_ = std::string(tr(STR_FONT_CLEANUP_FAILED)) + ": " + fileName;
    }

    if (canActivate && !Storage.rename(PREVIEW_NEXT_PATH, PREVIEW_TMP_PATH)) {
      if (hadPreview && Storage.rename(PREVIEW_BACKUP_PATH, PREVIEW_TMP_PATH)) {
        previewFontId_ =
            sdFontSystem.beginPreview(renderer, PREVIEW_TMP_PATH, families_[previousFamilyIndex].name.c_str(),
                                      families_[previousFamilyIndex].files[previousFileIndex].pointSize);
        if (previewFontId_ != 0) {
          activePreviewFamilyIndex_ = previousFamilyIndex;
          activePreviewFileIndex_ = previousFileIndex;
        }
      }
      canActivate = false;
      errorMessage_ = std::string(tr(STR_FONT_REPLACEMENT_FAILED)) + ": " + fileName;
    }

    if (canActivate) {
      previewFontId_ = sdFontSystem.beginPreview(renderer, PREVIEW_TMP_PATH, family.name.c_str(), file.pointSize);
      if (previewFontId_ == 0) {
        if (Storage.exists(PREVIEW_TMP_PATH)) Storage.remove(PREVIEW_TMP_PATH);
        if (hadPreview && Storage.rename(PREVIEW_BACKUP_PATH, PREVIEW_TMP_PATH)) {
          previewFontId_ =
              sdFontSystem.beginPreview(renderer, PREVIEW_TMP_PATH, families_[previousFamilyIndex].name.c_str(),
                                        families_[previousFamilyIndex].files[previousFileIndex].pointSize);
          if (previewFontId_ != 0) {
            activePreviewFamilyIndex_ = previousFamilyIndex;
            activePreviewFileIndex_ = previousFileIndex;
          }
        }
        canActivate = false;
        errorMessage_ = std::string(tr(STR_FONT_INVALID_FILE)) + ": " + fileName;
      }
    }

    if (!canActivate) {
      if (Storage.exists(PREVIEW_NEXT_PATH)) Storage.remove(PREVIEW_NEXT_PATH);
      if (hadPreview && previewFontId_ != 0) {
        activePreviewFamilyIndex_ = previousFamilyIndex;
        activePreviewFileIndex_ = previousFileIndex;
      }
      state_ = ERROR;
    } else {
      if (Storage.exists(PREVIEW_BACKUP_PATH) && !Storage.remove(PREVIEW_BACKUP_PATH)) {
        LOG_ERR("FONT", "Failed to remove previous preview backup");
      }
      activePreviewFamilyIndex_ = familyIndex;
      activePreviewFileIndex_ = fileIndex;
      state_ = FONT_PREVIEW;
    }
  }
  requestUpdateAndWait();
}

void FontDownloadActivity::installPreviewedFamily() {
  if (previewFamilyIndex_ < 0 || previewFamilyIndex_ >= static_cast<int>(families_.size())) {
    LOG_DBG("FONT", "INSTALL skip: bad family index %d (families=%d)", previewFamilyIndex_,
            static_cast<int>(families_.size()));
    return;
  }
  const int familyIndex = previewFamilyIndex_;
  const int fileIndex = previewFileIndex_;
  if (fileIndex < 0 || fileIndex >= static_cast<int>(families_[familyIndex].files.size())) {
    LOG_DBG("FONT", "INSTALL skip: bad file index %d (files=%d)", fileIndex,
            static_cast<int>(families_[familyIndex].files.size()));
    return;
  }
  LOG_DBG("FONT", "INSTALL_PREVIEW: fam=%d file=%d tmp=%d next=%d previewFontId=%d", familyIndex, fileIndex,
          Storage.exists(PREVIEW_TMP_PATH), Storage.exists(PREVIEW_NEXT_PATH), previewFontId_);
  {
    RenderLock lock(*this);
    if (previewFontId_ != 0) {
      sdFontSystem.endPreview(renderer);
      previewFontId_ = 0;
    }
    activePreviewFamilyIndex_ = -1;
    activePreviewFileIndex_ = -1;
    if (Storage.exists(PREVIEW_NEXT_PATH)) Storage.remove(PREVIEW_NEXT_PATH);
    if (Storage.exists(PREVIEW_BACKUP_PATH)) Storage.remove(PREVIEW_BACKUP_PATH);
  }
  currentFileIndex_ = 0;
  currentFileTotal_ = 1;
  downloadFamily(familyIndex, fileIndex, PREVIEW_TMP_PATH);
  requestUpdateAndWait();
}

void FontDownloadActivity::downloadFamily(int familyIndex, int fileIndex, const char* stagedFilePath) {
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return;
  // Work from a copy: beginNetworkTransfer(familyIndex) releases families_ for
  // the duration of each file transfer, and this copy stays valid throughout.
  ManifestFamily family = families_[familyIndex];
  const bool uiPackage = fileIndex == UI_PACKAGE_FILE_INDEX;
  const bool completeFamily = fileIndex < 0 && !uiPackage;
  if (fileIndex < UI_PACKAGE_FILE_INDEX ||
      (!completeFamily && !uiPackage && fileIndex >= static_cast<int>(family.files.size()))) {
    return;
  }
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = familyIndex;
    downloadingFileIndex_ = fileIndex;
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
    errorAction_ = ErrorAction::Install;
  }
  requestUpdateAndWait();

  uint64_t downloadBytes = 0;
  if (completeFamily) {
    downloadBytes = family.totalSize;
  } else if (uiPackage) {
    for (const auto& file : family.files) {
      if (isUiPointSize(file.pointSize)) downloadBytes += file.size;
    }
  } else if (!(stagedFilePath && Storage.exists(stagedFilePath))) {
    downloadBytes = family.files[fileIndex].size;
  }
  const uint64_t requiredBytes = downloadBytes + STORAGE_RESERVE_BYTES;
  const uint64_t freeBytes = Storage.freeBytes();
  if (freeBytes < requiredBytes) {
    LOG_ERR("FONT", "Insufficient storage: free=%llu required=%llu", static_cast<unsigned long long>(freeBytes),
            static_cast<unsigned long long>(requiredBytes));
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_INSUFFICIENT_STORAGE);
    return;
  }

  if (!fontInstaller_.ensureFamilyDir(family.name.c_str())) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_DIRECTORY_FAILED);
    return;
  }

  uint32_t transactionFingerprint = family.fingerprint;
  if (!completeFamily) {
    if (uiPackage) {
      transactionFingerprint = 2166136261U;
      for (const auto& file : family.files) {
        if (isUiPointSize(file.pointSize)) {
          transactionFingerprint =
              fingerprintBytes(&file.fingerprint, sizeof(file.fingerprint), transactionFingerprint);
        }
      }
    } else {
      transactionFingerprint = family.files[fileIndex].fingerprint;
    }
  }
  if (!fontInstaller_.beginFamilyInstall(family.name.c_str(), transactionFingerprint, completeFamily)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_REPLACEMENT_FAILED);
    return;
  }

  auto failTransaction = [&](const std::string& message, const bool cancelled) {
    const bool rollbackSucceeded = fontInstaller_.rollbackFamilyInstall(family.name.c_str());
    fontInstaller_.refreshRegistry();
    refreshFamilyState(families_[familyIndex]);
    RenderLock lock(*this);
    lowMemoryDownload_ = false;
    if (cancelled && rollbackSucceeded) {
      state_ = FAMILY_LIST;
      return;
    }
    state_ = ERROR;
    errorMessage_ = rollbackSucceeded ? message : tr(STR_FONT_ROLLBACK_FAILED);
  };

  const size_t firstFileIndex = completeFamily || uiPackage ? 0 : static_cast<size_t>(fileIndex);
  const size_t endFileIndex = completeFamily || uiPackage ? family.files.size() : firstFileIndex + 1;
  for (size_t i = firstFileIndex; i < endFileIndex; i++) {
    if (uiPackage && !isUiPointSize(family.files[i].pointSize)) continue;
    const auto& file = family.files[i];
    const std::string fileName = manifestFileName(family, file);

    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    char destPath[160];
    if (!FontInstaller::buildFontPath(family.name.c_str(), fileName.c_str(), destPath, sizeof(destPath))) {
      LOG_ERR("FONT", "Failed to build destination path for %s/%s", family.name.c_str(), fileName.c_str());
      failTransaction(tr(STR_FONT_DIRECTORY_FAILED), false);
      return;
    }

    const std::string finalPath = destPath;
    const std::string partPath = finalPath + ".part";

    if (Storage.exists(partPath.c_str()) && !Storage.remove(partPath.c_str())) {
      LOG_ERR("FONT", "Failed to remove stale partial download: %s", partPath.c_str());
      failTransaction(tr(STR_FONT_CLEANUP_FAILED), false);
      return;
    }
    const std::string legacyBackupPath = finalPath + ".bak";
    if (Storage.exists(legacyBackupPath.c_str())) {
      if (Storage.exists(finalPath.c_str())) {
        if (!Storage.remove(legacyBackupPath.c_str())) {
          failTransaction(tr(STR_FONT_REPLACEMENT_FAILED), false);
          return;
        }
      } else if (!Storage.rename(legacyBackupPath.c_str(), finalPath.c_str())) {
        failTransaction(tr(STR_FONT_ROLLBACK_FAILED), false);
        return;
      }
    }

    const bool useStagedFile = !completeFamily && !uiPackage && stagedFilePath && Storage.exists(stagedFilePath);
    LOG_DBG("FONT", "DLFAMILY decide: fam=%d file=%d staged='%s' useStaged=%d complete=%d", familyIndex,
            static_cast<int>(i), stagedFilePath ? stagedFilePath : "", useStagedFile, completeFamily);
    if (useStagedFile) {
      if (!Storage.rename(stagedFilePath, partPath.c_str())) {
        failTransaction(tr(STR_FONT_REPLACEMENT_FAILED), false);
        return;
      }
      fileProgress_ = file.size;
      requestUpdate(true);
    } else {
      std::string url = *file.baseUrl + fileName;

      // Release the full catalog before bringing WiFi back up. With all 44
      // families resident the X4 has only ~3.5 KB max-alloc, and WiFi startup
      // can abort before the transfer has a chance to release that memory.
      beginNetworkTransfer(familyIndex);
      if (!ensureWifiConnectedForTransfer()) {
        endNetworkTransfer();
        failTransaction(tr(STR_WIFI_CONN_FAILED), cancelRequested_);
        return;
      }
      auto result = HttpDownloader::downloadToFile(
          url, partPath.c_str(), [this](size_t downloaded, size_t total) { updateDownloadProgress(downloaded, total); },
          &cancelRequested_, "", "", [this] { return pollDownloadCancellation(); }, file.size);
      if (!endNetworkTransfer()) {
        RenderLock lock(*this);
        lowMemoryDownload_ = false;
        state_ = ERROR;
        errorMessage_ = tr(STR_FONT_LIST_READ_FAILED);
        return;
      }

      if (result == HttpDownloader::ABORTED) {
        failTransaction("", true);
        return;
      }

      if (result != HttpDownloader::OK) {
        LOG_ERR("FONT", "Download failed: %s (%d)", fileName.c_str(), result);
        failTransaction(std::string(tr(STR_DOWNLOAD_FAILED)) + ": " + fileName, false);
        return;
      }
      if (pollDownloadCancellation()) {
        failTransaction("", true);
        return;
      }
    }

    if (!useStagedFile) {
      HalFile downloadedFile;
      if (!Storage.openFileForRead("FONT", partPath.c_str(), downloadedFile)) {
        failTransaction(std::string(tr(STR_DOWNLOAD_FAILED)) + ": " + fileName, false);
        return;
      }
      const size_t actualSize = downloadedFile.fileSize();
      downloadedFile.close();
      if (actualSize != file.size) {
        LOG_ERR("FONT", "Size mismatch for %s: got %zu expected %zu", fileName.c_str(), actualSize, file.size);
        failTransaction(std::string(tr(STR_FONT_FILE_SIZE_MISMATCH)) + ": " + fileName, false);
        return;
      }

      std::array<uint8_t, SHA256_BYTES> actualSha256{};
      if (!computeFileSha256(partPath.c_str(), actualSha256)) {
        if (cancelRequested_) {
          failTransaction("", true);
          return;
        }
        LOG_ERR("FONT", "Failed to calculate SHA-256: %s", partPath.c_str());
        failTransaction(std::string(tr(STR_FONT_CHECKSUM_FAILED)) + ": " + fileName, false);
        return;
      }
      if (actualSha256 != file.sha256) {
        LOG_ERR("FONT", "SHA-256 mismatch for %s", fileName.c_str());
        failTransaction(std::string(tr(STR_FONT_CHECKSUM_MISMATCH)) + ": " + fileName, false);
        return;
      }
      if (pollDownloadCancellation()) {
        failTransaction("", true);
        return;
      }
      LOG_DBG("FONT", "Downloaded %s (size=%zu SHA-256 verified)", fileName.c_str(), file.size);

      if (!fontInstaller_.validateCpfontFile(partPath.c_str())) {
        LOG_ERR("FONT", "Invalid .cpfont: %s", partPath.c_str());
        failTransaction(std::string(tr(STR_FONT_INVALID_FILE)) + ": " + fileName, false);
        return;
      }
      if (pollDownloadCancellation()) {
        failTransaction("", true);
        return;
      }
    }

    if (!fontInstaller_.prepareFontReplacement(finalPath.c_str())) {
      failTransaction(tr(STR_FONT_REPLACEMENT_FAILED), false);
      return;
    }

    if (!Storage.rename(partPath.c_str(), finalPath.c_str())) {
      LOG_ERR("FONT", "Failed to install validated font: %s", finalPath.c_str());
      failTransaction(tr(STR_FONT_REPLACEMENT_FAILED), false);
      return;
    }
    currentFileIndex_++;
  }

  if (completeFamily) {
    std::vector<std::string> retainedFilenames;
    retainedFilenames.reserve(family.files.size());
    for (const auto& file : family.files) retainedFilenames.push_back(manifestFileName(family, file));
    if (!fontInstaller_.prepareFamilyPrune(family.name.c_str(), retainedFilenames)) {
      failTransaction(tr(STR_FONT_REPLACEMENT_FAILED), false);
      return;
    }
    if (pollDownloadCancellation()) {
      failTransaction("", true);
      return;
    }
  }

  if (!fontInstaller_.commitFamilyInstall(family.name.c_str())) {
    failTransaction(tr(STR_FONT_REPLACEMENT_FAILED), false);
    return;
  }
  const bool cleanupSucceeded = fontInstaller_.cleanupCommittedFamilyInstall(family.name.c_str());
  if (!cleanupSucceeded) {
    LOG_ERR("FONT", "Committed family cleanup deferred until recovery: %s", family.name.c_str());
  }

  const bool reloadResidentFonts =
      family.name == SETTINGS.sdFontFamilyName || family.name == SETTINGS.sdUiFontFamilyName;
  fontInstaller_.refreshRegistry();
  if (reloadResidentFonts) {
    sdFontSystem.markResidentFontsDirty();
    RenderLock lock(*this);
    sdFontSystem.ensureLoaded(renderer);
  }
  refreshFamilyState(families_[familyIndex]);
  LOG_DBG("FONT", "DLFAMILY complete: fam=%d installed", familyIndex);
  {
    RenderLock lock(*this);
    lowMemoryDownload_ = false;
    state_ = COMPLETE;
    errorAction_ = ErrorAction::None;
  }
}

bool FontDownloadActivity::isUiPointSize(const uint8_t pointSize) {
  return pointSize == 8 || pointSize == 10 || pointSize == 12;
}

int FontDownloadActivity::uiPackageFileCount(const ManifestFamily& family) const {
  return static_cast<int>(std::count_if(family.files.begin(), family.files.end(),
                                        [](const ManifestFile& file) { return isUiPointSize(file.pointSize); }));
}

int FontDownloadActivity::detailFileIndexForRow(const ManifestFamily& family, const int row) const {
  const int uiFileCount = uiPackageFileCount(family);
  if (uiFileCount > 0) {
    if (row == 0) return UI_PACKAGE_FILE_INDEX;
    int readerRow = 1;
    for (size_t i = 0; i < family.files.size(); ++i) {
      if (isUiPointSize(family.files[i].pointSize)) continue;
      if (readerRow++ == row) return static_cast<int>(i);
    }
    return -1;
  }
  return row >= 0 && row < static_cast<int>(family.files.size()) ? row : -1;
}

int FontDownloadActivity::detailRowForFileIndex(const ManifestFamily& family, const int fileIndex) const {
  if (fileIndex < 0 || fileIndex >= static_cast<int>(family.files.size())) return 0;
  const int uiFileCount = uiPackageFileCount(family);
  if (uiFileCount == 0) return fileIndex;
  if (isUiPointSize(family.files[fileIndex].pointSize)) return 0;
  int row = 1;
  for (int i = 0; i < fileIndex; ++i) {
    if (!isUiPointSize(family.files[i].pointSize)) ++row;
  }
  return row;
}
bool FontDownloadActivity::heapSufficientForNetworkTransfer() const {
  // TLS manifests and font files need a roughly contiguous arena. Thresholds
  // sit just below the worst successful transfer measured on-device
  // (35232/21492) so normal post-manifest runs never trip; cold-boot entries
  // with a fragmented heap do.
  return ESP.getFreeHeap() >= MIN_NETWORK_FREE_HEAP_BYTES && ESP.getMaxAllocHeap() >= MIN_NETWORK_MAX_ALLOC_BYTES;
}

bool FontDownloadActivity::detailHasDeleteRow() const {
  if (detailFamilyIndex_ < 0 || detailFamilyIndex_ >= static_cast<int>(families_.size())) return false;
  return families_[detailFamilyIndex_].installed;
}

int FontDownloadActivity::detailRowCount() const {
  if (detailFamilyIndex_ < 0 || detailFamilyIndex_ >= static_cast<int>(families_.size())) return 0;
  const auto& family = families_[detailFamilyIndex_];
  const int uiFileCount = uiPackageFileCount(family);
  int rows = static_cast<int>(family.files.size()) - uiFileCount + (uiFileCount > 0 ? 1 : 0);
  if (detailHasDeleteRow()) rows += 1;  // delete row
  return rows;
}

void FontDownloadActivity::promptDeleteFamily(int familyIndex) {
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) {
    return;
  }
  LOG_DBG("FONT", "PROMPT_DELETE: family=%d name='%s'", familyIndex, families_[familyIndex].name.c_str());
  detailFamilyIndex_ = familyIndex;  // reused as the pending-delete slot
  std::string heading = tr(STR_DELETE);
  const auto& family = families_[familyIndex];
  std::string body = family.name;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void FontDownloadActivity::promptDeleteSelectedFamily() { promptDeleteFamily(familyIndexFromList(selectedIndex_)); }

void FontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  // Deletion can originate from the family list or the family detail
  // (point-size) list; the target index is captured before the confirmation
  // dialog is pushed.
  auto& family = families_[detailFamilyIndex_];

  if (fontInstaller_.deleteFamily(family.name.c_str()) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_DELETE_FAILED);
  } else {
    sdFontSystem.markRegistryDirty();
    {
      RenderLock lock(*this);
      sdFontSystem.ensureLoaded(renderer);
    }
    family.installed = false;
    family.partial = false;
    family.hasUpdate = false;
  }

  requestUpdate();
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isFontReposRow(selectedIndex_) || isDownloadAllRow(selectedIndex_) || isUpdateAllRow(selectedIndex_)) {
    return false;
  }
  if (selectedIndex_ < specialRowCount() || selectedIndex_ >= listItemCount()) return false;
  return families_[familyIndexFromList(selectedIndex_)].installed;
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    auto activateSelected = [this] {
      if (isFontReposRow(selectedIndex_)) {
        openFontRepositories();
        return;
      }
      if (families_.empty()) return;
      if (isDownloadAllRow(selectedIndex_)) {
        currentFileIndex_ = 0;
        currentFileTotal_ = 0;
        for (const auto& f : families_) {
          if (!f.installed || f.partial) currentFileTotal_ += f.files.size();
        }
        downloadAll();
      } else if (isUpdateAllRow(selectedIndex_)) {
        currentFileIndex_ = 0;
        currentFileTotal_ = 0;
        for (const auto& f : families_) {
          if (f.hasUpdate) currentFileTotal_ += f.files.size();
        }
        updateAll();
      } else {
        const int familyIndex = familyIndexFromList(selectedIndex_);
        // Open the family's point-size list instead of downloading a default
        // size up front, so the user can pick which size(s) to install.
        detailFamilyIndex_ = familyIndex;
        selectedIndex_ = detailRowForFileIndex(families_[familyIndex], defaultPreviewFileIndex(families_[familyIndex]));
        LOG_DBG("FONT", "FAMILY_DETAIL enter: family=%d name='%s' files=%u defaultIdx=%d free=%u max=%u", familyIndex,
                families_[familyIndex].name.c_str(), static_cast<unsigned>(families_[familyIndex].files.size()),
                selectedIndex_, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        {
          RenderLock lock(*this);
          state_ = FAMILY_DETAIL;
        }
      }
      requestUpdateAndWait();
    };

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int listSize = listItemCount();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    int listTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    if (!catalogUpdatedAt_.empty()) listTop += lineHeight + metrics.verticalSpacing;
    const Rect listRect = GUI.getContentRect(renderer, listTop, metrics.verticalSpacing);
    const int pageItems = GUI.getListPageItems(listRect.height, true);

    if (listSize > 0) {
      switch (handleListTouch(selectedIndex_, listSize, listRect.y, listRect.height, true)) {
        case ListTouchResult::Activated:
          activateSelected();
          return;
        case ListTouchResult::Consumed:
          return;
        case ListTouchResult::None:
          break;
      }

      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up) {
        selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
        requestUpdate();
        return;
      }
      if (swipe == MappedInputManager::SwipeDir::Down) {
        selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
        requestUpdate();
        return;
      }
    }

    buttonNavigator_.onNextRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onPreviousRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      activateSelected();
      return;
    }
  } else if (state_ == FONT_PREVIEW) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      returnToFamilyList();
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      installPreviewedFamily();
      return;
    }

    if (previewFamilyIndex_ >= 0 && previewFamilyIndex_ < static_cast<int>(families_.size())) {
      const int fileCount = static_cast<int>(families_[previewFamilyIndex_].files.size());
      if (fileCount > 0) {
        auto changePreviewFile = [this, fileCount](const int newIndex) {
          if (newIndex != previewFileIndex_) {
            downloadPreview(previewFamilyIndex_, newIndex);
          }
        };

        const auto swipe = mappedInput.wasSwipe();
        if (swipe == MappedInputManager::SwipeDir::Up) {
          changePreviewFile(ButtonNavigator::previousIndex(previewFileIndex_, fileCount));
          return;
        }
        if (swipe == MappedInputManager::SwipeDir::Down) {
          changePreviewFile(ButtonNavigator::nextIndex(previewFileIndex_, fileCount));
          return;
        }

        buttonNavigator_.onPreviousRelease([this, changePreviewFile, fileCount] {
          changePreviewFile(ButtonNavigator::previousIndex(previewFileIndex_, fileCount));
        });
        buttonNavigator_.onNextRelease([this, changePreviewFile, fileCount] {
          changePreviewFile(ButtonNavigator::nextIndex(previewFileIndex_, fileCount));
        });
      }
    }
  } else if (state_ == FAMILY_DETAIL) {
    auto activateDetail = [this] {
      if (detailFamilyIndex_ < 0 || detailFamilyIndex_ >= static_cast<int>(families_.size())) return;
      const int deleteRow = detailRowCount() - 1;
      if (detailHasDeleteRow() && selectedIndex_ == deleteRow) {
        promptDeleteFamily(detailFamilyIndex_);
        return;
      }
      const auto& family = families_[detailFamilyIndex_];
      const int fileIndex = detailFileIndexForRow(family, selectedIndex_);
      if (fileIndex == UI_PACKAGE_FILE_INDEX) {
        currentFileIndex_ = 0;
        currentFileTotal_ = static_cast<size_t>(uiPackageFileCount(family));
        downloadFamily(detailFamilyIndex_, UI_PACKAGE_FILE_INDEX);
      } else if (fileIndex >= 0) {
        downloadPreview(detailFamilyIndex_, fileIndex);
      }
    };

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("FONT", "FAMILY_DETAIL back -> FAMILY_LIST (detailFamily=%d)", detailFamilyIndex_);
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
      return;
    }

    const int detailRows = detailRowCount();
    if (detailRows > 0) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const Rect contentRect = GUI.getContentRect(renderer, contentTop, metrics.verticalSpacing);
      switch (handleListTouch(selectedIndex_, detailRows, contentRect.y, contentRect.height, true)) {
        case ListTouchResult::Activated:
          activateDetail();
          return;
        case ListTouchResult::Consumed:
          return;
        case ListTouchResult::None:
          break;
      }

      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up) {
        selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, detailRows);
        requestUpdate();
        return;
      }
      if (swipe == MappedInputManager::SwipeDir::Down) {
        selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, detailRows);
        requestUpdate();
        return;
      }
    }

    buttonNavigator_.onNextRelease([this, detailRows] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, detailRows);
      requestUpdate();
    });
    buttonNavigator_.onPreviousRelease([this, detailRows] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, detailRows);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      activateDetail();
    }
  } else if (state_ == COMPLETE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      returnToFamilyList();
      requestUpdate();
      return;
    }

    auto retry = [this] {
      switch (errorAction_) {
        case ErrorAction::Preview:
          if (previewFamilyIndex_ >= 0 && previewFamilyIndex_ < static_cast<int>(families_.size())) {
            const auto& family = families_[previewFamilyIndex_];
            if (previewFileIndex_ >= 0 && previewFileIndex_ < static_cast<int>(family.files.size())) {
              downloadPreview(previewFamilyIndex_, previewFileIndex_);
              return;
            }
          }
          break;
        case ErrorAction::Install:
          if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
            const char* stagedFilePath =
                downloadingFileIndex_ >= 0 && Storage.exists(PREVIEW_TMP_PATH) ? PREVIEW_TMP_PATH : nullptr;
            downloadFamily(downloadingFamilyIndex_, downloadingFileIndex_, stagedFilePath);
            requestUpdateAndWait();
            return;
          }
          break;
        case ErrorAction::None:
          break;
      }
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        errorAction_ = ErrorAction::None;
      }
      requestUpdate();
    };

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      retry();
      return;
    }
    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      retry();
      return;
    }
  }
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  if (lowMemoryDownload_ && (state_ == DOWNLOADING_PREVIEW || state_ == DOWNLOADING)) {
    renderLowMemoryProgress();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_FONT_BROWSER));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int centerY = screen.y + (screen.height - lineHeight) / 2;
  downloadProgressBarY_ = centerY + metrics.verticalSpacing;
  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    if (listItemCount() == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      int listTop = contentTop;
      if (!catalogUpdatedAt_.empty()) {
        char updatedBuf[40];
        snprintf(updatedBuf, sizeof(updatedBuf), tr(STR_FONT_CATALOG_UPDATED_AT),
                 catalogUpdatedAt_.substr(0, 10).c_str());
        renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop, updatedBuf);
        listTop = contentTop + lineHeight + metrics.verticalSpacing;
      }
      const Rect listRect = GUI.getContentRect(renderer, listTop, metrics.verticalSpacing);
      GUI.drawList(
          renderer, listRect, listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (isFontReposRow(index)) {
              return std::string(tr(STR_FONT_REPOSITORIES));
            }
            if (isDownloadAllRow(index)) {
              return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
            }
            if (isUpdateAllRow(index)) {
              return std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
            }
            return families_[familyIndexFromList(index)].name;
          },
          [this](int index) -> std::string {
            if (isFontReposRow(index)) {
              if (partialManifestFailure_) return std::string(tr(STR_FONT_REPO_SOME_FAILED));
              char buf[64];
              snprintf(buf, sizeof(buf), tr(STR_FONT_REPO_COUNT), static_cast<int>(FONT_REPO_STORE.getCount()) + 1);
              return std::string(buf);
            }
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            return families_[familyIndexFromList(index)].description;
          },
          nullptr,
          [this](int index) -> std::string {
            if (isFontReposRow(index) || isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            const auto& f = families_[familyIndexFromList(index)];
            if (f.hasUpdate) return tr(STR_UPDATE_AVAILABLE);
            if (f.installed) return tr(STR_INSTALLED);
            return "";
          },
          true,
          [this](int index) -> bool {
            if (isFontReposRow(index) || isDownloadAllRow(index) || isUpdateAllRow(index)) return false;
            const auto& f = families_[familyIndexFromList(index)];
            return f.installed && !f.partial && !f.hasUpdate;
          });

      const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                                isFontReposRow(selectedIndex_)     ? tr(STR_SELECT)
                                                : isSelectedFamilyDeletable()      ? tr(STR_DELETE)
                                                : isDownloadAllRow(selectedIndex_) ? tr(STR_DOWNLOAD)
                                                : isUpdateAllRow(selectedIndex_)   ? tr(STR_UPDATE)
                                                                                   : tr(STR_PREVIEW),
                                                tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == FAMILY_DETAIL) {
    if (detailFamilyIndex_ < 0 || detailFamilyIndex_ >= static_cast<int>(families_.size())) {
      return;
    }
    const auto& detailFamily = families_[detailFamilyIndex_];
    const int detailRows = detailRowCount();
    if (detailRows == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      const Rect contentRect = GUI.getContentRect(renderer, contentTop, metrics.verticalSpacing);
      GUI.drawList(
          renderer, contentRect, detailRows, selectedIndex_,
          [this, &detailFamily, detailRows](int index) -> std::string {
            const bool isDeleteRow = detailHasDeleteRow() && index == detailRows - 1;
            if (isDeleteRow) return std::string(tr(STR_DELETE)) + " " + detailFamily.name;
            const int fileIndex = detailFileIndexForRow(detailFamily, index);
            if (fileIndex == UI_PACKAGE_FILE_INDEX) return tr(STR_EXT_UI_FONT);
            if (fileIndex < 0) return "";
            char buf[32];
            snprintf(buf, sizeof(buf), "%u pt", static_cast<unsigned>(detailFamily.files[fileIndex].pointSize));
            return std::string(buf);
          },
          [this, &detailFamily, detailRows](int index) -> std::string {
            if (detailHasDeleteRow() && index == detailRows - 1) return "";
            const int fileIndex = detailFileIndexForRow(detailFamily, index);
            if (fileIndex == UI_PACKAGE_FILE_INDEX) {
              size_t totalSize = 0;
              for (const auto& file : detailFamily.files) {
                if (isUiPointSize(file.pointSize)) totalSize += file.size;
              }
              return formatSize(totalSize);
            }
            return fileIndex >= 0 ? formatSize(detailFamily.files[fileIndex].size) : "";
          },
          nullptr,
          [this, &detailFamily, detailRows](int index) -> std::string {
            if (detailHasDeleteRow() && index == detailRows - 1) return "";
            const int fileIndex = detailFileIndexForRow(detailFamily, index);
            if (detailFamily.hasUpdate) return tr(STR_UPDATE_AVAILABLE);
            if (fileIndex == UI_PACKAGE_FILE_INDEX) {
              const bool installed = std::all_of(
                  detailFamily.files.begin(), detailFamily.files.end(),
                  [](const ManifestFile& file) { return !isUiPointSize(file.pointSize) || file.installed; });
              return detailFamily.installed && installed ? tr(STR_INSTALLED) : "";
            }
            if (fileIndex >= 0 && detailFamily.installed && detailFamily.files[fileIndex].installed) {
              return tr(STR_INSTALLED);
            }
            return "";
          },
          true);

      const bool deleteSelected = detailHasDeleteRow() && selectedIndex_ == detailRows - 1;
      const bool uiPackageSelected = detailFileIndexForRow(detailFamily, selectedIndex_) == UI_PACKAGE_FILE_INDEX;
      const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                                deleteSelected      ? tr(STR_DELETE)
                                                : uiPackageSelected ? tr(STR_DOWNLOAD)
                                                                    : tr(STR_PREVIEW),
                                                tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING_PREVIEW) {
    std::string statusText = tr(STR_DOWNLOADING);
    if (previewFamilyIndex_ >= 0 && previewFamilyIndex_ < static_cast<int>(families_.size())) {
      const auto& family = families_[previewFamilyIndex_];
      statusText += " " + family.name;
      if (previewFileIndex_ >= 0 && previewFileIndex_ < static_cast<int>(family.files.size())) {
        statusText += " " + std::to_string(family.files[previewFileIndex_].pointSize) + " pt";
      }
    }
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    const int barY = downloadProgressBarY_;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == FONT_PREVIEW) {
    if (previewFamilyIndex_ >= 0 && previewFamilyIndex_ < static_cast<int>(families_.size())) {
      const auto& family = families_[previewFamilyIndex_];
      if (previewFileIndex_ >= 0 && previewFileIndex_ < static_cast<int>(family.files.size())) {
        const auto& file = family.files[previewFileIndex_];
        char label[160];
        snprintf(label, sizeof(label), "%s %u pt", family.name.c_str(), static_cast<unsigned>(file.pointSize));
        const auto labelText = renderer.truncatedText(UI_10_FONT_ID, label, pageWidth - 2 * metrics.contentSidePadding);
        renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop, labelText.c_str());

        const int sampleFontId = previewFontId_ != 0 ? previewFontId_ : UI_10_FONT_ID;
        const int sampleLineHeight = renderer.getLineHeight(sampleFontId);
        const int maxY = screen.y + screen.height - metrics.verticalSpacing;
        int y = contentTop + lineHeight;
        auto* cache = previewFontId_ != 0 ? renderer.getFontCacheManager() : nullptr;
        bool prewarmed = true;
        if (cache) {
          cache->resetStats();
          LOG_DBG("FONT", "Preview prewarm start: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        }
        for (const char* sample : PREVIEW_SAMPLE_LINES) {
          if (y + sampleLineHeight > maxY) break;
          // A complete four-line CJK preview can exceed the largest contiguous
          // heap block after WiFi use. Warm and render one line at a time so the
          // persistent SD-font arena is reused without falling back to per-glyph IO.
          if (cache && !cache->prewarmCache(previewFontId_, sample, 0x01)) {
            prewarmed = false;
          }
          const auto text = renderer.truncatedText(sampleFontId, sample, pageWidth - 2 * metrics.contentSidePadding);
          renderer.drawText(sampleFontId, metrics.contentSidePadding, y, text.c_str());
          y += sampleLineHeight;
        }
        if (cache) {
          cache->logStats("font-preview");
          LOG_DBG("FONT", "Preview prewarm done: ok=%d free=%d max=%d", prewarmed, ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
        }
      }
    }
    const bool previewFileInstalled =
        previewFamilyIndex_ >= 0 && previewFamilyIndex_ < static_cast<int>(families_.size()) &&
        previewFileIndex_ >= 0 && previewFileIndex_ < static_cast<int>(families_[previewFamilyIndex_].files.size()) &&
        families_[previewFamilyIndex_].files[previewFileIndex_].installed;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), previewFileInstalled ? tr(STR_UPDATE) : tr(STR_DOWNLOAD),
                                              tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == DOWNLOADING) {
    const auto& family = families_[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + family.name + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    const int barY = downloadProgressBarY_;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (renderer.isDarkMode()) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBuffer();
  }
}

void FontDownloadActivity::renderLowMemoryProgress() {
  if (fileTotal_ == 0) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const Rect barRect{metrics.contentSidePadding, downloadProgressBarY_, pageWidth - metrics.contentSidePadding * 2,
                     metrics.progressBarHeight};
  const int percent = static_cast<int>((static_cast<uint64_t>(fileProgress_) * 100U) / fileTotal_);

  renderer.fillRect(barRect.x, barRect.y, barRect.width, barRect.height, false);
  renderer.drawRect(barRect.x, barRect.y, barRect.width, barRect.height);
  const int fillWidth = (barRect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(barRect.x + 2, barRect.y + 2, fillWidth, barRect.height - 4);
  }
  renderer.setPartialUpdateRect(barRect.x, barRect.y, barRect.width, barRect.height);
  if (renderer.isDarkMode()) {
    renderer.displayBufferDarkRedrive();
  } else {
    renderer.displayBuffer();
  }
}
