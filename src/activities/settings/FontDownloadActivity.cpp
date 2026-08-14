#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr size_t MAX_MANIFEST_BYTES = 32 * 1024;
constexpr size_t MAX_MANIFEST_FAMILIES = 32;
constexpr size_t MAX_FILES_PER_FAMILY = 32;
constexpr uint64_t STORAGE_RESERVE_BYTES = 8ULL * 1024ULL * 1024ULL;
constexpr const char* PREVIEW_TMP_PATH = "/.font_preview.cpfont";
constexpr const char* PREVIEW_NEXT_PATH = "/.font_preview_next.cpfont";
constexpr const char* PREVIEW_BACKUP_PATH = "/.font_preview_backup.cpfont";
constexpr uint8_t DEFAULT_PREVIEW_POINT_SIZE = 14;

constexpr const char* PREVIEW_SAMPLE_LINES[] = {
    "\xe9\x98\x85\xe8\xaf\xbb\xe9\xa2\x84\xe8\xa7\x88  "
    "\xe6\xb1\x89\xe5\xad\x97\xe6\x98\x8e\xe6\x9c\x9d\xe9\xbb\x91\xe4\xbd\x93\xe6\xa5\xb7\xe4\xbd\x93",
    "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87  "
    "\xe9\x96\xb1\xe8\xae\x80\xe9\xa0\x90\xe8\xa6\xbd\xe5\xad\x97\xe9\xab\x94",
    "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88\xe3\x81\x8a  "
    "\xe3\x82\xa2\xe3\x82\xa4\xe3\x82\xa6\xe3\x82\xa8\xe3\x82\xaa",
    "The quick brown fox 0123456789",
};
constexpr const char* PREVIEW_SAMPLE_TEXT =
    "\xe9\x98\x85\xe8\xaf\xbb\xe9\xa2\x84\xe8\xa7\x88\xe6\xb1\x89\xe5\xad\x97\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6"
    "\x96\x87\xe3\x81\x82\xe3\x82\xa2The quick brown fox 0123456789";
constexpr size_t MAX_STYLES_PER_FAMILY = 8;
constexpr size_t MAX_BASE_URL_LENGTH = 256;
constexpr size_t MAX_DESCRIPTION_LENGTH = 160;
constexpr size_t MAX_STYLE_NAME_LENGTH = 32;
constexpr size_t MAX_FONT_FILE_BYTES = 256ULL * 1024ULL * 1024ULL;

bool isValidBaseUrl(const std::string& url) {
  const bool isHttps = url.compare(0, 8, "https://") == 0;
  if (!isHttps || url.empty() || url.size() > MAX_BASE_URL_LENGTH || url.back() != '/') return false;
  if (url.find_first_of(" \t\r\n\\?#") != std::string::npos) return false;

  const size_t hostStart = 8;
  const size_t pathStart = url.find('/', hostStart);
  return pathStart != std::string::npos && pathStart > hostStart && url.find("..", pathStart) == std::string::npos;
}
}  // namespace

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();
  // ActivityManager invokes onExit() while holding the rendering mutex.
  closePreview();
  if (Storage.exists(PREVIEW_TMP_PATH)) Storage.remove(PREVIEW_TMP_PATH);
  if (Storage.exists(PREVIEW_NEXT_PATH)) Storage.remove(PREVIEW_NEXT_PATH);
  if (Storage.exists(PREVIEW_BACKUP_PATH)) Storage.remove(PREVIEW_BACKUP_PATH);

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
    delay(30);
  }
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  if (!fetchAndParseManifest()) {
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

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
  }
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

bool FontDownloadActivity::fetchAndParseManifest() {
  // Download manifest to a temp file on SD card to avoid holding both
  // TLS buffers and the full JSON string in RAM simultaneously.
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";

  cancelRequested_ = false;
  if (Storage.exists(MANIFEST_TMP) && !Storage.remove(MANIFEST_TMP)) {
    LOG_ERR("FONT", "Failed to remove stale font manifest");
    errorMessage_ = tr(STR_FONT_LIST_READ_FAILED);
    return false;
  }

  beginNetworkTransfer();
  const auto result = HttpDownloader::downloadToFile(
      FONT_MANIFEST_URL, MANIFEST_TMP, [this](size_t, size_t) { pollDownloadCancellation(); }, &cancelRequested_, "",
      "", [this] { return pollDownloadCancellation(); }, MAX_MANIFEST_BYTES);
  endNetworkTransfer();
  if (result == HttpDownloader::ABORTED) {
    Storage.remove(MANIFEST_TMP);
    return false;
  }
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = tr(STR_FONT_LIST_FETCH_FAILED);
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  // HTTP client is now closed and TLS buffers are freed. Parse JSON from file.
  HalFile manifestFile;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = tr(STR_FONT_LIST_READ_FAILED);
    return false;
  }

  const size_t manifestSize = manifestFile.fileSize();
  if (manifestSize == 0 || manifestSize > MAX_MANIFEST_BYTES) {
    LOG_ERR("FONT", "Manifest size is invalid: %zu", manifestSize);
    manifestFile.close();
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, manifestFile);
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);

  if (err) {
    LOG_ERR("FONT", "Manifest parse error: %s", err.c_str());
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }

  const int version = doc["version"] | 0;
  if (version != FONTS_MANIFEST_VERSION) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", version);
    errorMessage_ = tr(STR_FONT_MANIFEST_UNSUPPORTED);
    return false;
  }

  if (!doc["baseUrl"].is<const char*>() || !doc["families"].is<JsonArray>()) {
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }

  std::string parsedBaseUrl = doc["baseUrl"].as<const char*>();
  if (!isValidBaseUrl(parsedBaseUrl)) {
    LOG_ERR("FONT", "Invalid manifest base URL");
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }

  JsonArray familiesArr = doc["families"].as<JsonArray>();
  if (familiesArr.size() > MAX_MANIFEST_FAMILIES) {
    LOG_ERR("FONT", "Too many font families: %zu", familiesArr.size());
    errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
    return false;
  }

  std::vector<ManifestFamily> parsedFamilies;
  std::vector<std::string> manifestFilenames;
  parsedFamilies.reserve(familiesArr.size());
  fontInstaller_.refreshRegistry();

  for (JsonVariant familyValue : familiesArr) {
    if (!familyValue.is<JsonObject>()) {
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }
    JsonObject fObj = familyValue.as<JsonObject>();
    if (!fObj["name"].is<const char*>() || !fObj["files"].is<JsonArray>()) {
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }

    ManifestFamily family;
    family.name = fObj["name"].as<const char*>();
    if (!FontInstaller::isValidFamilyName(family.name.c_str())) {
      LOG_ERR("FONT", "Invalid family name in manifest");
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }
    for (const auto& existing : parsedFamilies) {
      if (existing.name == family.name) {
        LOG_ERR("FONT", "Duplicate family in manifest: %s", family.name.c_str());
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
    }

    if (!fObj["description"].isNull()) {
      if (!fObj["description"].is<const char*>()) {
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      family.description = fObj["description"].as<const char*>();
      if (family.description.size() > MAX_DESCRIPTION_LENGTH) {
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
    }

    if (!fObj["styles"].isNull()) {
      if (!fObj["styles"].is<JsonArray>()) {
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      JsonArray styles = fObj["styles"].as<JsonArray>();
      if (styles.size() > MAX_STYLES_PER_FAMILY) {
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      for (JsonVariant styleValue : styles) {
        if (!styleValue.is<const char*>()) {
          errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
          return false;
        }
        std::string style = styleValue.as<const char*>();
        if (style.empty() || style.size() > MAX_STYLE_NAME_LENGTH) {
          errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
          return false;
        }
        for (const auto& existing : family.styles) {
          if (existing == style) {
            errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
            return false;
          }
        }
        family.styles.push_back(std::move(style));
      }
    }

    JsonArray files = fObj["files"].as<JsonArray>();
    if (files.size() == 0 || files.size() > MAX_FILES_PER_FAMILY) {
      errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
      return false;
    }

    for (JsonVariant fileValue : files) {
      if (!fileValue.is<JsonObject>()) {
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      JsonObject fileObj = fileValue.as<JsonObject>();
      if (!fileObj["name"].is<const char*>() || !fileObj["size"].is<size_t>() || !fileObj["crc32"].is<uint32_t>()) {
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }

      ManifestFile file;
      file.name = fileObj["name"].as<const char*>();
      file.size = fileObj["size"].as<size_t>();
      file.crc32 = fileObj["crc32"].as<uint32_t>();
      if (!parsePointSize(file.name.c_str(), family.name.c_str(), file.pointSize)) {
        LOG_ERR("FONT", "Font filename does not match family/size convention: %s", file.name.c_str());
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      if (!FontInstaller::isValidCpfontFilename(file.name.c_str()) || file.size == 0 ||
          file.size > MAX_FONT_FILE_BYTES || file.size > std::numeric_limits<size_t>::max() - family.totalSize) {
        LOG_ERR("FONT", "Invalid file entry in manifest: %s", file.name.c_str());
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }
      for (const auto& existing : manifestFilenames) {
        if (existing == file.name) {
          LOG_ERR("FONT", "Duplicate file in manifest: %s", file.name.c_str());
          errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
          return false;
        }
      }

      for (const auto& existing : family.files) {
        if (existing.pointSize == file.pointSize) {
          LOG_ERR("FONT", "Duplicate point size in family %s: %u", family.name.c_str(), file.pointSize);
          errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
          return false;
        }
      }
      char path[160];
      if (!FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), path, sizeof(path))) {
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }

      family.totalSize += file.size;
      manifestFilenames.push_back(file.name);
      family.files.push_back(std::move(file));
      std::sort(family.files.begin(), family.files.end(),
                [](const ManifestFile& a, const ManifestFile& b) { return a.pointSize < b.pointSize; });
    }

    refreshFamilyState(family);
    parsedFamilies.push_back(std::move(family));
  }

  baseUrl_ = std::move(parsedBaseUrl);
  families_ = std::move(parsedFamilies);
  LOG_DBG("FONT", "Manifest loaded: %zu families", families_.size());
  return true;
}
bool FontDownloadActivity::pollDownloadCancellation() {
  mappedInput.update();
  if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    cancelRequested_ = true;
  }
  return cancelRequested_;
}

void FontDownloadActivity::beginNetworkTransfer() {
  RenderLock lock(*this);
  lowMemoryDownload_ = true;
  lastProgressPercent_ = -1;

  LOG_DBG("FONT", "Heap before font cache release: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  FontManager::getInstance().releaseGlyphCaches();
  if (auto* cache = renderer.getFontCacheManager()) {
    cache->clearCache();
  }
  LOG_DBG("FONT", "Heap before network transfer: free=%d max=%d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void FontDownloadActivity::endNetworkTransfer() {
  RenderLock lock(*this);
  lowMemoryDownload_ = false;
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
  family.installed = fontInstaller_.isFamilyInstalled(family.name.c_str());
  family.hasUpdate = false;
  if (!family.installed) return;

  for (const auto& file : family.files) {
    char path[160];
    if (!FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), path, sizeof(path))) {
      family.hasUpdate = true;
      return;
    }

    HalFile installedFile;
    if (!Storage.openFileForRead("FONT", path, installedFile)) {
      family.hasUpdate = true;
      return;
    }
    const size_t actualSize = installedFile.fileSize();
    installedFile.close();
    if (actualSize != file.size) {
      family.hasUpdate = true;
      return;
    }
  }
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed) continue;
    downloadFamily(families_[i]);
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
    downloadFamily(families_[i]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const auto& f : families_) {
    if (!f.installed) return true;
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
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool FontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool FontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int FontDownloadActivity::listItemCount() const {
  return families_.empty() ? 0 : static_cast<int>(families_.size()) + specialRowCount();
}

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed) {
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

// Standard CRC32 matching zlib/Python zlib.crc32().
bool FontDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  HalFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  const size_t fileSize = f.fileSize();
  size_t bytesRead = 0;
  while (bytesRead < fileSize) {
    if ((bytesRead & 0xFFFF) == 0 && pollDownloadCancellation()) {
      return false;
    }
    const size_t remaining = fileSize - bytesRead;
    const int n = f.read(buf, remaining < BUF_SIZE ? remaining : BUF_SIZE);
    if (n <= 0) {
      LOG_ERR("FONT", "Read failed during CRC check: %s", path);
      return false;
    }
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
    bytesRead += static_cast<size_t>(n);
  }
  outCrc = crc;
  return true;
}
void FontDownloadActivity::closePreview() {
  if (previewFontId_ != 0) {
    sdFontSystem.endPreview(renderer);
    previewFontId_ = 0;
  }
  activePreviewFamilyIndex_ = -1;
  activePreviewFileIndex_ = -1;
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

void FontDownloadActivity::returnToFamilyList() {
  RenderLock lock(*this);
  closePreview();
  state_ = FAMILY_LIST;
  errorAction_ = ErrorAction::None;
}

void FontDownloadActivity::downloadPreview(int familyIndex, int fileIndex) {
  if (state_ == DOWNLOADING_PREVIEW || state_ == DOWNLOADING) return;
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return;
  auto& family = families_[familyIndex];
  if (fileIndex < 0 || fileIndex >= static_cast<int>(family.files.size())) return;
  const auto& file = family.files[fileIndex];
  const bool hadPreview = previewFontId_ != 0;
  const int previousFamilyIndex = activePreviewFamilyIndex_;
  const int previousFileIndex = activePreviewFileIndex_;

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

  auto failPreview = [this, &file, hadPreview, previousFamilyIndex, previousFileIndex](const char* message,
                                                                                       bool cancelled) {
    if (Storage.exists(PREVIEW_NEXT_PATH)) Storage.remove(PREVIEW_NEXT_PATH);
    RenderLock lock(*this);
    if (cancelled) {
      if (hadPreview) {
        previewFamilyIndex_ = activePreviewFamilyIndex_;
        previewFileIndex_ = activePreviewFileIndex_;
        state_ = FONT_PREVIEW;
      } else {
        state_ = FAMILY_LIST;
      }
      errorAction_ = ErrorAction::None;
    } else {
      state_ = ERROR;
      errorMessage_ = std::string(message) + ": " + file.name;
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

  beginNetworkTransfer();
  const auto result = HttpDownloader::downloadToFile(
      baseUrl_ + file.name, PREVIEW_NEXT_PATH,
      [this](size_t downloaded, size_t total) { updateDownloadProgress(downloaded, total); }, &cancelRequested_, "", "",
      [this] { return pollDownloadCancellation(); }, file.size);
  endNetworkTransfer();
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

  uint32_t actualCrc = 0;
  if (!computeFileCrc32(PREVIEW_NEXT_PATH, actualCrc)) {
    if (cancelRequested_) {
      failPreview("", true);
      requestUpdateAndWait();
      return;
    }
    failPreview(tr(STR_FONT_CHECKSUM_FAILED), false);
    requestUpdateAndWait();
    return;
  }
  if (actualCrc != file.crc32) {
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
      errorMessage_ = std::string(tr(STR_FONT_CLEANUP_FAILED)) + ": " + file.name;
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
        errorMessage_ = std::string(tr(STR_FONT_REPLACEMENT_FAILED)) + ": " + file.name;
      }
    } else if (canActivate && Storage.exists(PREVIEW_TMP_PATH) && !Storage.remove(PREVIEW_TMP_PATH)) {
      canActivate = false;
      errorMessage_ = std::string(tr(STR_FONT_CLEANUP_FAILED)) + ": " + file.name;
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
      errorMessage_ = std::string(tr(STR_FONT_REPLACEMENT_FAILED)) + ": " + file.name;
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
        errorMessage_ = std::string(tr(STR_FONT_INVALID_FILE)) + ": " + file.name;
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
      if (auto* cache = renderer.getFontCacheManager()) {
        cache->prewarmCache(previewFontId_, PREVIEW_SAMPLE_TEXT, 0x01);
      }
      state_ = FONT_PREVIEW;
    }
  }
  requestUpdateAndWait();
}

void FontDownloadActivity::installPreviewedFamily() {
  if (previewFamilyIndex_ < 0 || previewFamilyIndex_ >= static_cast<int>(families_.size())) return;
  const int familyIndex = previewFamilyIndex_;
  {
    RenderLock lock(*this);
    closePreview();
  }
  currentFileIndex_ = 0;
  currentFileTotal_ = families_[familyIndex].files.size();
  downloadFamily(families_[familyIndex]);
  requestUpdateAndWait();
}

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  struct FileTransaction {
    std::string finalPath;
    std::string partPath;
    std::string backupPath;
    bool backupCreated = false;
    bool replacementInstalled = false;
  };

  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
    errorAction_ = ErrorAction::Install;
  }
  requestUpdateAndWait();

  const uint64_t requiredBytes = static_cast<uint64_t>(family.totalSize) + STORAGE_RESERVE_BYTES;
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

  std::vector<FileTransaction> transactions;
  transactions.reserve(family.files.size());

  auto rollback = [&transactions]() {
    bool success = true;
    for (auto it = transactions.rbegin(); it != transactions.rend(); ++it) {
      if (Storage.exists(it->partPath.c_str()) && !Storage.remove(it->partPath.c_str())) {
        LOG_ERR("FONT", "Failed to remove partial download during rollback: %s", it->partPath.c_str());
        success = false;
      }

      if (it->backupCreated) {
        if (Storage.exists(it->finalPath.c_str()) && !Storage.remove(it->finalPath.c_str())) {
          LOG_ERR("FONT", "Failed to remove replacement during rollback: %s", it->finalPath.c_str());
          success = false;
          continue;
        }
        if (!Storage.rename(it->backupPath.c_str(), it->finalPath.c_str())) {
          LOG_ERR("FONT", "Failed to restore backup: %s", it->backupPath.c_str());
          success = false;
        }
      } else if (it->replacementInstalled && Storage.exists(it->finalPath.c_str()) &&
                 !Storage.remove(it->finalPath.c_str())) {
        LOG_ERR("FONT", "Failed to remove new font during rollback: %s", it->finalPath.c_str());
        success = false;
      }
    }
    return success;
  };

  auto failTransaction = [&](const std::string& message, const bool cancelled) {
    const bool rollbackSucceeded = rollback();
    fontInstaller_.refreshRegistry();
    refreshFamilyState(family);
    RenderLock lock(*this);
    if (cancelled && rollbackSucceeded) {
      state_ = FAMILY_LIST;
      return;
    }
    state_ = ERROR;
    errorMessage_ = rollbackSucceeded ? message : tr(STR_FONT_ROLLBACK_FAILED);
  };

  for (size_t i = 0; i < family.files.size(); i++) {
    const auto& file = family.files[i];

    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    char destPath[160];
    if (!FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), destPath, sizeof(destPath))) {
      LOG_ERR("FONT", "Failed to build destination path for %s/%s", family.name.c_str(), file.name.c_str());
      failTransaction(tr(STR_FONT_DIRECTORY_FAILED), false);
      return;
    }

    FileTransaction transaction;
    transaction.finalPath = destPath;
    transaction.partPath = transaction.finalPath + ".part";
    transaction.backupPath = transaction.finalPath + ".bak";
    transactions.push_back(std::move(transaction));
    auto& current = transactions.back();

    if (Storage.exists(current.partPath.c_str()) && !Storage.remove(current.partPath.c_str())) {
      LOG_ERR("FONT", "Failed to remove stale partial download: %s", current.partPath.c_str());
      failTransaction(tr(STR_FONT_CLEANUP_FAILED), false);
      return;
    }
    if (Storage.exists(current.backupPath.c_str())) {
      if (Storage.exists(current.finalPath.c_str())) {
        if (!Storage.remove(current.backupPath.c_str())) {
          failTransaction(tr(STR_FONT_REPLACEMENT_FAILED), false);
          return;
        }
      } else if (!Storage.rename(current.backupPath.c_str(), current.finalPath.c_str())) {
        failTransaction(tr(STR_FONT_ROLLBACK_FAILED), false);
        return;
      }
    }

    std::string url = baseUrl_ + file.name;

    beginNetworkTransfer();
    auto result = HttpDownloader::downloadToFile(
        url, current.partPath.c_str(),
        [this](size_t downloaded, size_t total) { updateDownloadProgress(downloaded, total); }, &cancelRequested_, "",
        "", [this] { return pollDownloadCancellation(); }, file.size);
    endNetworkTransfer();

    if (result == HttpDownloader::ABORTED) {
      failTransaction("", true);
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name.c_str(), result);
      failTransaction(std::string(tr(STR_DOWNLOAD_FAILED)) + ": " + file.name, false);
      return;
    }
    if (pollDownloadCancellation()) {
      failTransaction("", true);
      return;
    }

    HalFile downloadedFile;
    if (!Storage.openFileForRead("FONT", current.partPath.c_str(), downloadedFile)) {
      failTransaction(std::string(tr(STR_DOWNLOAD_FAILED)) + ": " + file.name, false);
      return;
    }
    const size_t actualSize = downloadedFile.fileSize();
    downloadedFile.close();
    if (actualSize != file.size) {
      LOG_ERR("FONT", "Size mismatch for %s: got %zu expected %zu", file.name.c_str(), actualSize, file.size);
      failTransaction(std::string(tr(STR_FONT_FILE_SIZE_MISMATCH)) + ": " + file.name, false);
      return;
    }

    uint32_t actualCrc = 0;
    if (!computeFileCrc32(current.partPath.c_str(), actualCrc)) {
      if (cancelRequested_) {
        failTransaction("", true);
        return;
      }
      LOG_ERR("FONT", "Failed to open file for CRC check: %s", current.partPath.c_str());
      failTransaction(std::string(tr(STR_FONT_CHECKSUM_FAILED)) + ": " + file.name, false);
      return;
    }
    if (actualCrc != file.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", file.name.c_str(), actualCrc, file.crc32);
      failTransaction(std::string(tr(STR_FONT_CHECKSUM_MISMATCH)) + ": " + file.name, false);
      return;
    }
    if (pollDownloadCancellation()) {
      failTransaction("", true);
      return;
    }
    LOG_DBG("FONT", "Downloaded %s (size=%zu crc32=%08x)", file.name.c_str(), file.size, actualCrc);

    if (!fontInstaller_.validateCpfontFile(current.partPath.c_str())) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", current.partPath.c_str());
      failTransaction(std::string(tr(STR_FONT_INVALID_FILE)) + ": " + file.name, false);
      return;
    }
    if (pollDownloadCancellation()) {
      failTransaction("", true);
      return;
    }

    if (Storage.exists(current.finalPath.c_str())) {
      if (!Storage.rename(current.finalPath.c_str(), current.backupPath.c_str())) {
        LOG_ERR("FONT", "Failed to back up existing font: %s", current.finalPath.c_str());
        failTransaction(tr(STR_FONT_REPLACEMENT_FAILED), false);
        return;
      }
      current.backupCreated = true;
    }

    if (!Storage.rename(current.partPath.c_str(), current.finalPath.c_str())) {
      LOG_ERR("FONT", "Failed to install validated font: %s", current.finalPath.c_str());
      failTransaction(tr(STR_FONT_REPLACEMENT_FAILED), false);
      return;
    }
    current.replacementInstalled = true;
    currentFileIndex_++;
  }

  bool cleanupSucceeded = true;
  for (const auto& transaction : transactions) {
    if (Storage.exists(transaction.partPath.c_str()) && !Storage.remove(transaction.partPath.c_str())) {
      LOG_ERR("FONT", "Failed to remove partial download: %s", transaction.partPath.c_str());
      cleanupSucceeded = false;
    }
    if (transaction.backupCreated && Storage.exists(transaction.backupPath.c_str()) &&
        !Storage.remove(transaction.backupPath.c_str())) {
      LOG_ERR("FONT", "Failed to remove font backup: %s", transaction.backupPath.c_str());
      cleanupSucceeded = false;
    }
  }

  const bool reloadResidentFonts =
      family.name == SETTINGS.sdFontFamilyName || family.name == SETTINGS.sdUiFontFamilyName;
  fontInstaller_.refreshRegistry();
  if (reloadResidentFonts) {
    sdFontSystem.markResidentFontsDirty();
    RenderLock lock(*this);
    sdFontSystem.ensureLoaded(renderer);
  }
  refreshFamilyState(family);

  {
    RenderLock lock(*this);
    if (cleanupSucceeded) {
      state_ = COMPLETE;
      errorAction_ = ErrorAction::None;
    } else {
      state_ = ERROR;
      errorMessage_ = tr(STR_FONT_CLEANUP_FAILED);
    }
  }
}

void FontDownloadActivity::promptDeleteSelectedFamily() {
  const int pendingDeleteFamilyIndex = familyIndexFromList(selectedIndex_);
  if (pendingDeleteFamilyIndex < 0 || pendingDeleteFamilyIndex >= static_cast<int>(families_.size())) {
    return;
  }

  std::string heading = tr(STR_DELETE);
  const auto& family = families_[pendingDeleteFamilyIndex];
  std::string body = family.name;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void FontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  auto& family = families_[familyIndexFromList(selectedIndex_)];

  if (fontInstaller_.deleteFamily(family.name.c_str()) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to delete font";
  } else {
    sdFontSystem.markRegistryDirty();
    {
      RenderLock lock(*this);
      sdFontSystem.ensureLoaded(renderer);
    }
    family.installed = false;
    family.hasUpdate = false;
  }

  requestUpdate();
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isDownloadAllRow(selectedIndex_) || isUpdateAllRow(selectedIndex_)) return false;
  if (selectedIndex_ < specialRowCount() || selectedIndex_ >= listItemCount()) return false;
  const auto& family = families_[familyIndexFromList(selectedIndex_)];
  return family.installed && !family.hasUpdate;
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    auto activateSelected = [this] {
      if (families_.empty()) return;
      if (isDownloadAllRow(selectedIndex_)) {
        currentFileIndex_ = 0;
        currentFileTotal_ = 0;
        for (const auto& f : families_) {
          if (!f.installed) currentFileTotal_ += f.files.size();
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
        auto& family = families_[familyIndex];
        if (!family.installed || family.hasUpdate) {
          downloadPreview(familyIndex, defaultPreviewFileIndex(family));
        } else {
          promptDeleteSelectedFamily();
          return;
        }
      }
      requestUpdateAndWait();
    };

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int listSize = listItemCount();
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

    if (!families_.empty()) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int contentHeight =
          renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
      switch (handleListTouch(selectedIndex_, listSize, contentTop, contentHeight, true)) {
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
            downloadFamily(families_[downloadingFamilyIndex_]);
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

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;
  downloadProgressBarY_ = centerY + metrics.verticalSpacing;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    if (families_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (isDownloadAllRow(index)) {
              return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
            }
            if (isUpdateAllRow(index)) {
              return std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
            }
            return families_[familyIndexFromList(index)].name;
          },
          [this](int index) -> std::string {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            return families_[familyIndexFromList(index)].description;
          },
          nullptr,
          [this](int index) -> std::string {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            const auto& f = families_[familyIndexFromList(index)];
            if (f.hasUpdate) return tr(STR_UPDATE_AVAILABLE);
            if (f.installed) return tr(STR_INSTALLED);
            return "";
          },
          true,
          [this](int index) -> bool {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return false;
            const auto& f = families_[familyIndexFromList(index)];
            return f.installed && !f.hasUpdate;
          });

      const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                                isSelectedFamilyDeletable()        ? tr(STR_DELETE)
                                                : isDownloadAllRow(selectedIndex_) ? tr(STR_DOWNLOAD)
                                                : isUpdateAllRow(selectedIndex_)   ? tr(STR_UPDATE)
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
        const int maxY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
        int y = contentTop + lineHeight;
        for (const char* sample : PREVIEW_SAMPLE_LINES) {
          if (y + sampleLineHeight > maxY) break;
          const auto text = renderer.truncatedText(sampleFontId, sample, pageWidth - 2 * metrics.contentSidePadding);
          renderer.drawText(sampleFontId, metrics.contentSidePadding, y, text.c_str());
          y += sampleLineHeight;
        }
      }
    }
    const bool previewHasUpdate = previewFamilyIndex_ >= 0 &&
                                  previewFamilyIndex_ < static_cast<int>(families_.size()) &&
                                  families_[previewFamilyIndex_].hasUpdate;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), previewHasUpdate ? tr(STR_UPDATE) : tr(STR_DOWNLOAD),
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
