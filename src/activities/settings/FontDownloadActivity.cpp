#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include <limits>
#include <utility>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr size_t MAX_MANIFEST_BYTES = 32 * 1024;
constexpr size_t MAX_MANIFEST_FAMILIES = 32;
constexpr size_t MAX_FILES_PER_FAMILY = 32;
constexpr size_t MAX_STYLES_PER_FAMILY = 8;
constexpr size_t MAX_BASE_URL_LENGTH = 256;
constexpr size_t MAX_DESCRIPTION_LENGTH = 160;
constexpr size_t MAX_STYLE_NAME_LENGTH = 32;
constexpr size_t MAX_FONT_FILE_BYTES = 256ULL * 1024ULL * 1024ULL;

bool isValidBaseUrl(const std::string& url) {
  const bool isHttp = url.compare(0, 7, "http://") == 0;
  const bool isHttps = url.compare(0, 8, "https://") == 0;
  if ((!isHttp && !isHttps) || url.empty() || url.size() > MAX_BASE_URL_LENGTH || url.back() != '/') return false;
  if (url.find_first_of(" \t\r\n\\?#") != std::string::npos) return false;

  const size_t hostStart = isHttps ? 8 : 7;
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

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
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

  const auto result = HttpDownloader::downloadToFile(
      FONT_MANIFEST_URL, MANIFEST_TMP, [this](size_t, size_t) { pollDownloadCancellation(); }, &cancelRequested_, "",
      "", [this] { return pollDownloadCancellation(); });
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

      char path[160];
      if (!FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), path, sizeof(path))) {
        errorMessage_ = tr(STR_FONT_MANIFEST_INVALID);
        return false;
      }

      family.totalSize += file.size;
      manifestFilenames.push_back(file.name);
      family.files.push_back(std::move(file));
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

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  static constexpr uint64_t STORAGE_RESERVE_BYTES = 8ULL * 1024ULL * 1024ULL;

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

    auto result = HttpDownloader::downloadToFile(
        url, current.partPath.c_str(),
        [this](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          fileTotal_ = total;
          pollDownloadCancellation();
          requestUpdate(true);
        },
        &cancelRequested_, "", "", [this] { return pollDownloadCancellation(); });

    if (result == HttpDownloader::ABORTED) {
      failTransaction("", true);
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name.c_str(), result);
      failTransaction(std::string(tr(STR_DOWNLOAD_FAILED)) + ": " + file.name, false);
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
      LOG_ERR("FONT", "Failed to open file for CRC check: %s", current.partPath.c_str());
      failTransaction(std::string(tr(STR_FONT_CHECKSUM_FAILED)) + ": " + file.name, false);
      return;
    }
    if (actualCrc != file.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", file.name.c_str(), actualCrc, file.crc32);
      failTransaction(std::string(tr(STR_FONT_CHECKSUM_MISMATCH)) + ": " + file.name, false);
      return;
    }
    LOG_DBG("FONT", "Downloaded %s (size=%zu crc32=%08x)", file.name.c_str(), file.size, actualCrc);

    if (!fontInstaller_.validateCpfontFile(current.partPath.c_str())) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", current.partPath.c_str());
      failTransaction(std::string(tr(STR_FONT_INVALID_FILE)) + ": " + file.name, false);
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
        auto& family = families_[familyIndexFromList(selectedIndex_)];
        if (!family.installed || family.hasUpdate) {
          currentFileIndex_ = 0;
          currentFileTotal_ = family.files.size();
          downloadFamily(family);
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
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
        downloadFamily(families_[downloadingFamilyIndex_]);
        requestUpdateAndWait();
        return;
      } else {
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
        }
        requestUpdate();
      }
    } else {
      int x = 0;
      int y = 0;
      if (mappedInput.wasScreenTapped(x, y)) {
        if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
          downloadFamily(families_[downloadingFamilyIndex_]);
          requestUpdateAndWait();
          return;
        }
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
        }
        requestUpdate();
      }
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
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

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
                                                isSelectedFamilyDeletable()      ? tr(STR_DELETE)
                                                : isUpdateAllRow(selectedIndex_) ? tr(STR_UPDATE)
                                                                                 : tr(STR_DOWNLOAD),
                                                tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    const auto& family = families_[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + family.name + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
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

  renderer.displayBuffer();
}
