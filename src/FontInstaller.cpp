#include "FontInstaller.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "CpfontValidator.h"
#include "CrossPointSettings.h"

FontInstaller::FontInstaller(SdCardFontRegistry& registry) : registry_(registry) {}

namespace {

constexpr char kBackupSuffix[] = ".bak";
constexpr char kPartialSuffix[] = ".part";
constexpr char kTransactionBackupSuffix[] = ".txn.bak";
constexpr char kTransactionNewSuffix[] = ".txn.new";
constexpr char kInstallingMarker[] = ".crosspoint-installing";
constexpr char kCommittedMarker[] = ".crosspoint-committed";
constexpr char kInstalledReceipt[] = ".crosspoint-installed";
constexpr char kInstalledReceiptTemp[] = ".crosspoint-installed.tmp";
constexpr uint8_t kTransactionRecordMagic[] = {'C', 'P', 'T', 'X'};
constexpr size_t kTransactionRecordSize = sizeof(kTransactionRecordMagic) + 1 + sizeof(uint32_t);

enum class TransactionArtifact { None, Partial, Backup, NewFile };

bool hasSuffix(const std::string& value, const char* suffix) {
  const size_t suffixLength = strlen(suffix);
  return value.size() > suffixLength && value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
}

bool buildFamilyPath(const char* familyName, char* path, size_t pathSize) {
  if (!FontInstaller::isValidFamilyName(familyName) || path == nullptr || pathSize == 0) return false;
  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();
  const int pathLength = snprintf(path, pathSize, "%s/%s", root, familyName);
  return pathLength >= 0 && static_cast<size_t>(pathLength) < pathSize;
}

bool buildMarkerPath(const char* familyName, const char* markerName, char* path, size_t pathSize) {
  char familyPath[160];
  if (!buildFamilyPath(familyName, familyPath, sizeof(familyPath))) return false;
  const int pathLength = snprintf(path, pathSize, "%s/%s", familyPath, markerName);
  return pathLength >= 0 && static_cast<size_t>(pathLength) < pathSize;
}

bool createMarker(const char* path) {
  HalFile marker;
  if (!Storage.openFileForWrite("FONT", path, marker)) return false;
  static constexpr uint8_t kMarkerVersion = 1;
  const bool written = marker.write(kMarkerVersion) == 1;
  marker.flush();
  const bool closed = marker.close();
  return written && closed && Storage.exists(path);
}

bool readTransactionRecord(const char* path, uint32_t& fingerprint);

bool writeTransactionRecord(const char* path, uint32_t fingerprint) {
  uint8_t record[kTransactionRecordSize];
  memcpy(record, kTransactionRecordMagic, sizeof(kTransactionRecordMagic));
  record[sizeof(kTransactionRecordMagic)] = 1;
  const size_t fingerprintOffset = sizeof(kTransactionRecordMagic) + 1;
  for (size_t i = 0; i < sizeof(fingerprint); ++i) {
    record[fingerprintOffset + i] = static_cast<uint8_t>(fingerprint >> (i * 8));
  }

  HalFile file;
  if (!Storage.openFileForWrite("FONT", path, file)) return false;
  const bool written = file.write(record, sizeof(record)) == sizeof(record);
  file.flush();
  const bool closed = file.close();
  if (!written || !closed || !Storage.exists(path)) return false;

  uint32_t storedFingerprint = 0;
  return readTransactionRecord(path, storedFingerprint) && storedFingerprint == fingerprint;
}

bool readTransactionRecord(const char* path, uint32_t& fingerprint) {
  if (!Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("FONT", path, file) || file.fileSize() != kTransactionRecordSize) {
    if (file) file.close();
    return false;
  }
  uint8_t record[kTransactionRecordSize];
  const bool read = file.read(record, sizeof(record)) == static_cast<int>(sizeof(record));
  file.close();
  if (!read || memcmp(record, kTransactionRecordMagic, sizeof(kTransactionRecordMagic)) != 0 ||
      record[sizeof(kTransactionRecordMagic)] != 1) {
    return false;
  }

  fingerprint = 0;
  const size_t fingerprintOffset = sizeof(kTransactionRecordMagic) + 1;
  for (size_t i = 0; i < sizeof(fingerprint); ++i) {
    fingerprint |= static_cast<uint32_t>(record[fingerprintOffset + i]) << (i * 8);
  }
  return true;
}

bool findTransactionArtifact(const char* familyPath, std::string& path, TransactionArtifact& artifact) {
  HalFile dir = Storage.open(familyPath);
  if (!dir || !dir.isDirectory()) return false;

  char nameBuffer[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    const size_t nameLength = entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();
    if (nameLength == 0 || nameLength >= sizeof(nameBuffer)) continue;

    const std::string name(nameBuffer);
    const char* suffix = nullptr;
    if (hasSuffix(name, kTransactionBackupSuffix)) {
      artifact = TransactionArtifact::Backup;
      suffix = kTransactionBackupSuffix;
    } else if (hasSuffix(name, kTransactionNewSuffix)) {
      artifact = TransactionArtifact::NewFile;
      suffix = kTransactionNewSuffix;
    } else if (hasSuffix(name, kPartialSuffix)) {
      artifact = TransactionArtifact::Partial;
      suffix = kPartialSuffix;
    } else {
      continue;
    }

    const std::string finalName = name.substr(0, name.size() - strlen(suffix));
    if (!FontInstaller::isValidCpfontFilename(finalName.c_str())) continue;
    path = std::string(familyPath) + "/" + name;
    dir.close();
    return true;
  }
  dir.close();
  artifact = TransactionArtifact::None;
  return false;
}

bool hasFamilyTransactionArtifacts(const char* familyPath) {
  HalFile dir = Storage.open(familyPath);
  if (!dir || !dir.isDirectory()) return false;
  char nameBuffer[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    const size_t nameLength = entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();
    if (nameLength == 0 || nameLength >= sizeof(nameBuffer)) continue;
    const std::string name(nameBuffer);
    const char* suffix = hasSuffix(name, kTransactionBackupSuffix) ? kTransactionBackupSuffix
                         : hasSuffix(name, kTransactionNewSuffix)  ? kTransactionNewSuffix
                                                                   : nullptr;
    if (!suffix) continue;
    const std::string finalName = name.substr(0, name.size() - strlen(suffix));
    if (FontInstaller::isValidCpfontFilename(finalName.c_str())) {
      dir.close();
      return true;
    }
  }
  dir.close();
  return false;
}

bool processFamilyTransactionArtifacts(const char* familyPath, bool rollback) {
  std::string path;
  TransactionArtifact artifact = TransactionArtifact::None;
  while (findTransactionArtifact(familyPath, path, artifact)) {
    const char* suffix = kPartialSuffix;
    if (artifact == TransactionArtifact::Backup) {
      suffix = kTransactionBackupSuffix;
    } else if (artifact == TransactionArtifact::NewFile) {
      suffix = kTransactionNewSuffix;
    }
    const std::string finalPath = path.substr(0, path.size() - strlen(suffix));

    if (artifact == TransactionArtifact::Backup && rollback) {
      if (Storage.exists(finalPath.c_str()) && !Storage.remove(finalPath.c_str())) {
        LOG_ERR("FONT", "Failed to remove replacement during family rollback: %s", finalPath.c_str());
        return false;
      }
      if (!Storage.rename(path.c_str(), finalPath.c_str())) {
        LOG_ERR("FONT", "Failed to restore family backup: %s", path.c_str());
        return false;
      }
      continue;
    }

    if (artifact == TransactionArtifact::NewFile && rollback && Storage.exists(finalPath.c_str()) &&
        !Storage.remove(finalPath.c_str())) {
      LOG_ERR("FONT", "Failed to remove new font during family rollback: %s", finalPath.c_str());
      return false;
    }
    if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
      LOG_ERR("FONT", "Failed to remove family transaction file: %s", path.c_str());
      return false;
    }
  }
  return true;
}

bool finalizeCommittedFamily(const char* familyName, const char* familyPath, const char* committedPath) {
  uint32_t fingerprint = 0;
  if (!readTransactionRecord(committedPath, fingerprint)) {
    LOG_ERR("FONT", "Invalid committed family transaction record: %s", familyName);
    return false;
  }

  char receiptPath[192];
  char receiptTempPath[192];
  snprintf(receiptPath, sizeof(receiptPath), "%s/%s", familyPath, kInstalledReceipt);
  snprintf(receiptTempPath, sizeof(receiptTempPath), "%s/%s", familyPath, kInstalledReceiptTemp);
  if (Storage.exists(receiptTempPath) && !Storage.remove(receiptTempPath)) return false;
  if (!writeTransactionRecord(receiptTempPath, fingerprint)) return false;
  if (Storage.exists(receiptPath) && !Storage.remove(receiptPath)) return false;
  if (!Storage.rename(receiptTempPath, receiptPath)) return false;

  if (!processFamilyTransactionArtifacts(familyPath, false)) return false;
  return Storage.remove(committedPath);
}

bool recoverFamilyTransaction(const char* root, const char* familyName) {
  char familyPath[160];
  const int familyPathLength = snprintf(familyPath, sizeof(familyPath), "%s/%s", root, familyName);
  if (familyPathLength < 0 || static_cast<size_t>(familyPathLength) >= sizeof(familyPath)) return false;

  char installingPath[192];
  char committedPath[192];
  snprintf(installingPath, sizeof(installingPath), "%s/%s", familyPath, kInstallingMarker);
  snprintf(committedPath, sizeof(committedPath), "%s/%s", familyPath, kCommittedMarker);
  const bool installing = Storage.exists(installingPath);
  const bool committed = Storage.exists(committedPath);

  if (installing || (!committed && hasFamilyTransactionArtifacts(familyPath))) {
    if (!processFamilyTransactionArtifacts(familyPath, true)) return false;
    if (Storage.exists(committedPath) && !Storage.remove(committedPath)) return false;
    if (Storage.exists(installingPath) && !Storage.remove(installingPath)) return false;
    LOG_INF("FONT", "Rolled back interrupted family install: %s", familyName);
  } else if (committed) {
    if (!finalizeCommittedFamily(familyName, familyPath, committedPath)) return false;
    LOG_INF("FONT", "Finished committed family cleanup: %s", familyName);
  }
  return true;
}

bool findInterruptedFile(const char* familyPath, std::string& path, bool& isBackup) {
  HalFile dir = Storage.open(familyPath);
  if (!dir || !dir.isDirectory()) return false;

  char nameBuffer[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    const size_t nameLength = entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();
    if (nameLength == 0 || nameLength >= sizeof(nameBuffer)) continue;

    const std::string name(nameBuffer);
    bool foundBackup = false;
    size_t suffixLength = 0;
    if (hasSuffix(name, kBackupSuffix)) {
      foundBackup = true;
      suffixLength = strlen(kBackupSuffix);
    } else if (hasSuffix(name, kPartialSuffix)) {
      suffixLength = strlen(kPartialSuffix);
    } else {
      continue;
    }

    const std::string finalName = name.substr(0, name.size() - suffixLength);
    if (!FontInstaller::isValidCpfontFilename(finalName.c_str())) continue;

    path = std::string(familyPath) + "/" + name;
    isBackup = foundBackup;
    dir.close();
    return true;
  }
  dir.close();
  return false;
}

bool processInterruptedFiles(const char* root, const char* familyName) {
  char familyPath[160];
  const int familyPathLength = snprintf(familyPath, sizeof(familyPath), "%s/%s", root, familyName);
  if (familyPathLength < 0 || static_cast<size_t>(familyPathLength) >= sizeof(familyPath)) return false;

  std::string path;
  bool isBackup = false;
  while (findInterruptedFile(familyPath, path, isBackup)) {
    if (isBackup) {
      const std::string finalPath = path.substr(0, path.size() - strlen(kBackupSuffix));
      if (Storage.exists(finalPath.c_str())) {
        if (!Storage.remove(path.c_str())) {
          LOG_ERR("FONT", "Failed to remove committed font backup: %s", path.c_str());
          return false;
        }
      } else if (!Storage.rename(path.c_str(), finalPath.c_str())) {
        LOG_ERR("FONT", "Failed to restore interrupted font backup: %s", path.c_str());
        return false;
      } else {
        LOG_INF("FONT", "Restored interrupted font replacement: %s", finalPath.c_str());
      }
    } else if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
      LOG_ERR("FONT", "Failed to remove interrupted font download: %s", path.c_str());
      return false;
    }
  }
  return true;
}

bool recoverRoot(const char* root) {
  HalFile rootDir = Storage.open(root);
  if (!rootDir) return true;
  if (!rootDir.isDirectory()) return false;

  std::vector<std::string> familyNames;
  familyNames.reserve(SdCardFontRegistry::MAX_SD_FAMILIES);
  char nameBuffer[128];
  while (familyNames.size() < SdCardFontRegistry::MAX_SD_FAMILIES) {
    HalFile entry = rootDir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      const size_t nameLength = entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();
      if (nameLength > 0 && nameLength < sizeof(nameBuffer) && FontInstaller::isValidFamilyName(nameBuffer)) {
        familyNames.emplace_back(nameBuffer);
      }
    } else {
      entry.close();
    }
  }
  rootDir.close();

  bool success = true;
  for (const auto& familyName : familyNames) {
    if (!recoverFamilyTransaction(root, familyName.c_str())) {
      success = false;
      continue;
    }
    if (!processInterruptedFiles(root, familyName.c_str())) success = false;
  }
  return success;
}

}  // namespace

bool FontInstaller::isValidFamilyName(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  const size_t nameLen = strlen(name);
  if (nameLen > MAX_FAMILY_NAME_LENGTH) return false;

  // Reject path traversal
  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

  for (const char* p = name; *p != '\0'; ++p) {
    char c = *p;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

bool FontInstaller::isValidCpfontFilename(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  const size_t nameLen = strlen(name);
  if (nameLen > MAX_CPFONT_FILENAME_LENGTH) return false;

  // Reject path separators / traversal up front. Anything that could escape
  // the family directory or refer to a different one is a hard reject.
  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

  // Must end with ".cpfont" exactly.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  if (nameLen <= kExtLen) return false;
  if (strcmp(name + nameLen - kExtLen, kExt) != 0) return false;

  // Basename (before .cpfont) must be alphanumeric + hyphen + underscore only.
  // No additional dots keeps stray "Foo.cpfont.tmp"-style names out.
  size_t baseLen = nameLen - kExtLen;
  for (size_t i = 0; i < baseLen; ++i) {
    char c = name[i];
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

bool FontInstaller::recoverInterruptedInstalls() {
  bool success = recoverRoot(SdCardFontRegistry::FONTS_DIR_HIDDEN);
  if (!recoverRoot(SdCardFontRegistry::FONTS_DIR_VISIBLE)) success = false;
  return success;
}

bool FontInstaller::beginFamilyInstall(const char* familyName, uint32_t fingerprint) {
  char installingPath[192];
  char committedPath[192];
  char receiptTempPath[192];
  if (!buildMarkerPath(familyName, kInstallingMarker, installingPath, sizeof(installingPath)) ||
      !buildMarkerPath(familyName, kCommittedMarker, committedPath, sizeof(committedPath)) ||
      !buildMarkerPath(familyName, kInstalledReceiptTemp, receiptTempPath, sizeof(receiptTempPath))) {
    return false;
  }
  if (Storage.exists(installingPath) || Storage.exists(committedPath)) {
    LOG_ERR("FONT", "Unrecovered family transaction exists: %s", familyName);
    return false;
  }
  if (Storage.exists(receiptTempPath) && !Storage.remove(receiptTempPath)) return false;
  if (!writeTransactionRecord(installingPath, fingerprint)) {
    LOG_ERR("FONT", "Failed to create family transaction marker: %s", familyName);
    if (Storage.exists(installingPath) && !Storage.remove(installingPath)) {
      LOG_ERR("FONT", "Failed to remove incomplete family transaction marker: %s", familyName);
    }
    return false;
  }
  return true;
}

bool FontInstaller::prepareFontReplacement(const char* finalPath) {
  if (finalPath == nullptr || finalPath[0] == '\0') return false;
  const std::string backupPath = std::string(finalPath) + kTransactionBackupSuffix;
  const std::string newPath = std::string(finalPath) + kTransactionNewSuffix;
  if (Storage.exists(backupPath.c_str()) || Storage.exists(newPath.c_str())) return false;

  if (Storage.exists(finalPath)) {
    if (!Storage.rename(finalPath, backupPath.c_str())) {
      LOG_ERR("FONT", "Failed to preserve font for family rollback: %s", finalPath);
      return false;
    }
    return true;
  }

  if (!createMarker(newPath.c_str())) {
    LOG_ERR("FONT", "Failed to record new font for family rollback: %s", finalPath);
    return false;
  }
  return true;
}

bool FontInstaller::commitFamilyInstall(const char* familyName) {
  char installingPath[192];
  char committedPath[192];
  if (!buildMarkerPath(familyName, kInstallingMarker, installingPath, sizeof(installingPath)) ||
      !buildMarkerPath(familyName, kCommittedMarker, committedPath, sizeof(committedPath))) {
    return false;
  }
  const bool installing = Storage.exists(installingPath);
  const bool committed = Storage.exists(committedPath);
  if (committed && !installing) return true;
  if (!installing || committed) return false;
  if (!Storage.rename(installingPath, committedPath)) {
    // A FAT rename can reach durable storage even when the API reports an IO
    // error. Treat the observable committed state as authoritative.
    if (Storage.exists(committedPath) && !Storage.exists(installingPath)) {
      LOG_INF("FONT", "Family transaction committed despite rename error: %s", familyName);
      return true;
    }
    LOG_ERR("FONT", "Failed to commit family transaction: %s", familyName);
    return false;
  }
  return Storage.exists(committedPath) && !Storage.exists(installingPath);
}

bool FontInstaller::rollbackFamilyInstall(const char* familyName) {
  char familyPath[160];
  char installingPath[192];
  char committedPath[192];
  if (!buildFamilyPath(familyName, familyPath, sizeof(familyPath)) ||
      !buildMarkerPath(familyName, kInstallingMarker, installingPath, sizeof(installingPath)) ||
      !buildMarkerPath(familyName, kCommittedMarker, committedPath, sizeof(committedPath))) {
    return false;
  }
  if (Storage.exists(committedPath) && !Storage.exists(installingPath)) {
    LOG_ERR("FONT", "Refusing to roll back committed family transaction: %s", familyName);
    return false;
  }
  bool success = processFamilyTransactionArtifacts(familyPath, true);
  if (Storage.exists(committedPath) && !Storage.remove(committedPath)) success = false;
  if (Storage.exists(installingPath) && !Storage.remove(installingPath)) success = false;
  return success;
}

bool FontInstaller::cleanupCommittedFamilyInstall(const char* familyName) {
  char familyPath[160];
  char installingPath[192];
  char committedPath[192];
  if (!buildFamilyPath(familyName, familyPath, sizeof(familyPath)) ||
      !buildMarkerPath(familyName, kInstallingMarker, installingPath, sizeof(installingPath)) ||
      !buildMarkerPath(familyName, kCommittedMarker, committedPath, sizeof(committedPath))) {
    return false;
  }
  if (Storage.exists(installingPath) || !Storage.exists(committedPath)) return false;
  return finalizeCommittedFamily(familyName, familyPath, committedPath);
}

bool FontInstaller::installedFamilyMatches(const char* familyName, uint32_t fingerprint) {
  char receiptPath[192];
  if (!buildMarkerPath(familyName, kInstalledReceipt, receiptPath, sizeof(receiptPath))) return false;
  uint32_t installedFingerprint = 0;
  return readTransactionRecord(receiptPath, installedFingerprint) && installedFingerprint == fingerprint;
}

bool FontInstaller::ensureFamilyDir(const char* familyName) {
  if (!isValidFamilyName(familyName)) {
    LOG_ERR("FONT", "Invalid font family name");
    return false;
  }

  // Reuse the family's existing root if installed; otherwise pick the
  // default-write root (hidden if no roots exist yet).
  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();

  if (!Storage.exists(root)) {
    if (!Storage.mkdir(root)) {
      LOG_ERR("FONT", "Failed to create fonts dir: %s", root);
      return false;
    }
  }

  char dirPath[160];
  const int pathLength = snprintf(dirPath, sizeof(dirPath), "%s/%s", root, familyName);
  if (pathLength < 0 || static_cast<size_t>(pathLength) >= sizeof(dirPath)) {
    LOG_ERR("FONT", "Font family path is too long: %s", familyName);
    return false;
  }

  if (!Storage.exists(dirPath)) {
    if (!Storage.mkdir(dirPath)) {
      LOG_ERR("FONT", "Failed to create family dir: %s", dirPath);
      return false;
    }
  }
  return true;
}

bool FontInstaller::validateCpfontFile(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("FONT", path, file)) {
    LOG_ERR("FONT", "Cannot open for validation: %s", path);
    return false;
  }

  const auto readAt = [](void* context, size_t offset, void* destination, size_t length) -> int {
    auto* file = static_cast<HalFile*>(context);
    if (!file->seekSet(offset)) return -1;
    return file->read(destination, length);
  };
  const CpfontValidator::Reader reader{&file, file.fileSize(), readAt};
  const bool valid = CpfontValidator::validateV4(reader);
  file.close();
  if (!valid) LOG_ERR("FONT", "Invalid CPFONT v4 structure: %s", path);
  return valid;
}

bool FontInstaller::buildFontPath(const char* family, const char* filename, char* outBuf, size_t outBufSize) {
  if (outBuf == nullptr || outBufSize == 0) return false;
  outBuf[0] = '\0';
  if (!isValidFamilyName(family) || !isValidCpfontFilename(filename)) return false;

  // Use the same root selection as ensureFamilyDir: existing install dir wins,
  // otherwise the default-write root.
  const char* root = SdCardFontRegistry::findFamilyRoot(family);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();
  const int pathLength = snprintf(outBuf, outBufSize, "%s/%s/%s", root, family, filename);
  if (pathLength < 0 || static_cast<size_t>(pathLength) >= outBufSize) {
    outBuf[0] = '\0';
    return false;
  }
  return true;
}

FontInstaller::Error FontInstaller::deleteFamily(const char* familyName) {
  if (!isValidFamilyName(familyName)) {
    return Error::INVALID_FAMILY_NAME;
  }

  // A family may exist in either root (or, edge case, both). Remove from both.
  const char* roots[] = {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE};
  bool removedAny = false;
  bool sawAny = false;
  for (const char* root : roots) {
    char dirPath[160];
    snprintf(dirPath, sizeof(dirPath), "%s/%s", root, familyName);
    if (!Storage.exists(dirPath)) continue;
    sawAny = true;
    if (!Storage.removeDir(dirPath)) {
      LOG_ERR("FONT", "Failed to remove family dir: %s", dirPath);
      return Error::SD_WRITE_ERROR;
    }
    removedAny = true;
  }

  if (!sawAny) {
    LOG_DBG("FONT", "Family not found in any fonts root: %s", familyName);
    return Error::OK;  // Already gone
  }
  (void)removedAny;

  // If this was the active font, clear the setting
  bool settingsChanged = false;
  if (strcmp(SETTINGS.sdFontFamilyName, familyName) == 0) {
    SETTINGS.sdFontFamilyName[0] = '\0';
    settingsChanged = true;
  }
  if (strcmp(SETTINGS.sdUiFontFamilyName, familyName) == 0) {
    SETTINGS.sdUiFontFamilyName[0] = '\0';
    settingsChanged = true;
  }
  if (settingsChanged) {
    SETTINGS.saveToFile();
    LOG_DBG("FONT", "Cleared active SD font slots (deleted family: %s)", familyName);
  }

  return Error::OK;
}

void FontInstaller::refreshRegistry() { registry_.discover(); }

bool FontInstaller::isFamilyInstalled(const char* familyName) const {
  return registry_.findFamily(familyName) != nullptr;
}
