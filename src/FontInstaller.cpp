#include "FontInstaller.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

FontInstaller::FontInstaller(SdCardFontRegistry& registry) : registry_(registry) {}

namespace {

constexpr char kBackupSuffix[] = ".bak";
constexpr char kPartialSuffix[] = ".part";

bool hasSuffix(const std::string& value, const char* suffix) {
  const size_t suffixLength = strlen(suffix);
  return value.size() > suffixLength && value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
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

  uint8_t magic[CPFONT_MAGIC_LEN];
  size_t bytesRead = file.read(magic, CPFONT_MAGIC_LEN);
  file.close();

  if (bytesRead < CPFONT_MAGIC_LEN) {
    LOG_ERR("FONT", "File too small: %s (%zu bytes)", path, bytesRead);
    return false;
  }

  if (memcmp(magic, "CPFONT\0\0", CPFONT_MAGIC_LEN) != 0) {
    LOG_ERR("FONT", "Bad magic in: %s", path);
    return false;
  }

  return true;
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
