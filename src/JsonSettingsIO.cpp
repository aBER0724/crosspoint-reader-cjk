#include "JsonSettingsIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SettingsList.h"
#include "WifiCredentialStore.h"

namespace {

String tempPathFor(const char* path) { return String(path) + ".tmp"; }

String backupPathFor(const char* path) { return String(path) + ".bak"; }

bool readJsonFile(const char* tag, const String& path, String& json) {
  if (!Storage.exists(path.c_str())) {
    return false;
  }

  json = Storage.readFile(path.c_str());
  if (json.isEmpty()) {
    LOG_ERR(tag, "JSON file is empty or unreadable: %s", path.c_str());
    return false;
  }
  return true;
}

void recoverInterruptedJsonWrite(const char* tag, const char* path) {
  const String targetPath(path);
  const String tempPath = tempPathFor(path);
  const String backupPath = backupPathFor(path);

  if (Storage.exists(targetPath.c_str())) {
    if (Storage.exists(tempPath.c_str())) {
      Storage.remove(tempPath.c_str());
      LOG_DBG(tag, "Removed stale JSON temp file: %s", tempPath.c_str());
    }
    return;
  }

  if (Storage.exists(tempPath.c_str())) {
    if (Storage.rename(tempPath.c_str(), targetPath.c_str())) {
      LOG_ERR(tag, "Recovered interrupted JSON write from temp: %s", targetPath.c_str());
      return;
    }
    LOG_ERR(tag, "Failed to recover JSON temp file: %s", tempPath.c_str());
  }

  if (Storage.exists(backupPath.c_str())) {
    if (Storage.rename(backupPath.c_str(), targetPath.c_str())) {
      LOG_ERR(tag, "Recovered JSON backup: %s", targetPath.c_str());
    } else {
      LOG_ERR(tag, "Failed to recover JSON backup: %s", backupPath.c_str());
    }
  }
}

bool restoreJsonBackup(const char* tag, const char* path) {
  const String targetPath(path);
  const String backupPath = backupPathFor(path);

  if (!Storage.exists(backupPath.c_str())) {
    return false;
  }

  if (Storage.exists(targetPath.c_str())) {
    Storage.remove(targetPath.c_str());
  }

  if (!Storage.rename(backupPath.c_str(), targetPath.c_str())) {
    LOG_ERR(tag, "Failed to restore JSON backup: %s", backupPath.c_str());
    return false;
  }

  LOG_ERR(tag, "Restored JSON backup after load failure: %s", targetPath.c_str());
  return true;
}

template <typename Loader>
bool loadJsonWithBackup(const char* tag, const char* path, Loader loader, bool* needsResave) {
  if (path == nullptr || path[0] == '\0') {
    return false;
  }

  recoverInterruptedJsonWrite(tag, path);

  String json;
  if (readJsonFile(tag, path, json) && loader(json, needsResave)) {
    return true;
  }

  const String backupPath = backupPathFor(path);
  String backupJson;
  bool backupNeedsResave = false;
  if (!readJsonFile(tag, backupPath, backupJson) || !loader(backupJson, needsResave ? &backupNeedsResave : nullptr)) {
    return false;
  }

  if (restoreJsonBackup(tag, path) && needsResave) {
    *needsResave = backupNeedsResave;
  }
  return true;
}

bool writeFileAtomic(const char* path, const String& content) {
  if (path == nullptr || path[0] == '\0') {
    return false;
  }

  const String targetPath(path);
  const String tempPath = tempPathFor(path);
  const String backupPath = backupPathFor(path);

  Storage.remove(tempPath.c_str());
  if (!Storage.writeFile(tempPath.c_str(), content)) {
    Storage.remove(tempPath.c_str());
    return false;
  }

  if (Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }

  const bool hadExisting = Storage.exists(targetPath.c_str());
  if (hadExisting && !Storage.rename(targetPath.c_str(), backupPath.c_str())) {
    Storage.remove(tempPath.c_str());
    return false;
  }

  if (!Storage.rename(tempPath.c_str(), targetPath.c_str())) {
    Storage.remove(tempPath.c_str());
    if (hadExisting && Storage.exists(backupPath.c_str())) {
      Storage.rename(backupPath.c_str(), targetPath.c_str());
    }
    return false;
  }

  return true;
}

}  // namespace

bool JsonSettingsIO::jsonFileOrBackupExists(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    return false;
  }

  return Storage.exists(path) || Storage.exists(backupPathFor(path).c_str()) ||
         Storage.exists(tempPathFor(path).c_str());
}

// Convert legacy settings.
void applyLegacyStatusBarSettings(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::STATUS_BAR_MODE>(settings.statusBar)) {
    case CrossPointSettings::NONE:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::NO_PROGRESS:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::ONLY_BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::CHAPTER_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::CHAPTER_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::FULL:
    default:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
  }
}

// ---- CrossPointState ----

bool JsonSettingsIO::saveState(const CrossPointState& s, const char* path) {
  JsonDocument doc;
  doc["openEpubPath"] = s.openEpubPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < CrossPointState::SLEEP_RECENT_COUNT; i++) recentArr.add(s.recentSleepImages[i]);
  doc["recentSleepPos"] = s.recentSleepPos;
  doc["recentSleepFill"] = s.recentSleepFill;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;

  String json;
  serializeJson(doc, json);
  return writeFileAtomic(path, json);
}

bool JsonSettingsIO::loadState(CrossPointState& s, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  s.openEpubPath = doc["openEpubPath"] | std::string("");
  memset(s.recentSleepImages, 0, sizeof(s.recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount = recentArr.isNull() ? 0
                                             : std::min(static_cast<int>(recentArr.size()),
                                                        static_cast<int>(CrossPointState::SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) s.recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  s.recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (s.recentSleepPos >= CrossPointState::SLEEP_RECENT_COUNT)
    s.recentSleepPos = actualCount > 0 ? s.recentSleepPos % CrossPointState::SLEEP_RECENT_COUNT : 0;
  s.recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  s.recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(s.recentSleepFill), actualCount));
  // Migrate legacy single-image field from old state.json (pre-recency-buffer).
  // Only seeds the buffer if the new buffer is empty (fresh migration, not a resave).
  if (s.recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) s.pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  s.readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  s.lastSleepFromReader = doc["lastSleepFromReader"] | false;
  return true;
}

bool JsonSettingsIO::loadStateFile(CrossPointState& s, const char* path) {
  return loadJsonWithBackup(
      "CPS", path, [&s](const String& json, bool*) { return loadState(s, json.c_str()); }, nullptr);
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  JsonDocument doc;

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringPtr && !info.stringGetter) continue;

    if (info.stringGetter) {
      // Dynamic string settings (KOReader credentials etc.) are saved in their own files
      continue;
    } else if (info.stringPtr) {
      if (info.stringMaxLen == 0) continue;
      doc[info.key] = info.stringPtr;
    } else if (info.valueGetter) {
      // Dynamic enum value — skip (not stored directly in settings JSON)
      continue;
    } else if (info.valuePtr) {
      doc[info.key] = s.*(info.valuePtr);
    }
  }

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;

  String json;
  serializeJson(doc, json);
  return writeFileAtomic(path, json);
}

bool JsonSettingsIO::loadSettings(CrossPointSettings& s, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  // Legacy migration: if statusBarChapterPageCount is absent this is a pre-refactor settings file.
  // Populate s with migrated values now so the generic loop below picks them up as defaults and clamps them.
  if (doc["statusBarChapterPageCount"].isNull()) {
    applyLegacyStatusBarSettings(s);
  }

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringPtr && !info.stringGetter) continue;

    if (info.stringGetter) {
      // Dynamic string settings are loaded from their own files — skip.
      continue;
    } else if (info.stringPtr) {
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        info.stringPtr[0] = '\0';
        if (needsResave) *needsResave = true;
        continue;
      }
      const std::string fieldDefault(info.stringPtr);
      std::string val = doc[info.key] | fieldDefault;
      strncpy(info.stringPtr, val.c_str(), info.stringMaxLen - 1);
      info.stringPtr[info.stringMaxLen - 1] = '\0';
    } else if (info.valueGetter) {
      // Dynamic enum value — skip (not stored directly in settings JSON)
      continue;
    } else if (info.valuePtr) {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        v = clamp(v, (uint8_t)info.enumValues.size(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  }

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  using S = CrossPointSettings;
  s.frontButtonBack =
      clamp(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirm = clamp(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM, S::FRONT_BUTTON_HARDWARE_COUNT,
                               S::FRONT_HW_CONFIRM);
  s.frontButtonLeft =
      clamp(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
  CrossPointSettings::validateFrontButtonMapping(s);
  // Legacy lineSpacing migration: old enum values (TIGHT=0, NORMAL=1, WIDE=2) and
  // legacy slider values (20..60) must be converted to the new percent-based format.
  {
    const uint8_t rawLineSpacing = doc["lineSpacing"] | (uint8_t)S::LINE_SPACING_DEFAULT;
    if (rawLineSpacing < S::LINE_COMPRESSION_COUNT) {
      if (needsResave) *needsResave = true;
      switch (rawLineSpacing) {
        case S::TIGHT:
          s.lineSpacing = 90;
          break;
        case S::WIDE:
          s.lineSpacing = 120;
          break;
        case S::NORMAL:
        default:
          s.lineSpacing = 100;
          break;
      }
    } else if (rawLineSpacing >= 20 && rawLineSpacing <= 60) {
      // Legacy 20..60 slider values migrate to default 1.0x.
      if (needsResave) *needsResave = true;
      s.lineSpacing = S::LINE_SPACING_DEFAULT;
    }
    // Modern values (LINE_SPACING_MIN..LINE_SPACING_MAX) already loaded by SettingsList loop.
  }

  LOG_DBG("CPS", "Settings loaded from file");

  return true;
}

bool JsonSettingsIO::loadSettingsFile(CrossPointSettings& s, const char* path, bool* needsResave) {
  return loadJsonWithBackup(
      "CPS", path, [&s](const String& json, bool* resave) { return loadSettings(s, json.c_str(), resave); },
      needsResave);
}

// ---- KOReaderCredentialStore ----

bool JsonSettingsIO::saveKOReader(const KOReaderCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["username"] = store.getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(store.getPassword());
  doc["serverUrl"] = store.getServerUrl();
  doc["matchMethod"] = static_cast<uint8_t>(store.getMatchMethod());

  String json;
  serializeJson(doc, json);
  return writeFileAtomic(path, json);
}

bool JsonSettingsIO::loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("KRS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.username = doc["username"] | std::string("");
  bool ok = false;
  store.password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
  if (!ok || store.password.empty()) {
    store.password = doc["password"] | std::string("");
    if (!store.password.empty() && needsResave) *needsResave = true;
  }
  store.serverUrl = doc["serverUrl"] | std::string("");
  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  store.matchMethod = static_cast<DocumentMatchMethod>(method);

  LOG_DBG("KRS", "Loaded KOReader credentials for user: %s", store.username.c_str());
  return true;
}

bool JsonSettingsIO::loadKOReaderFile(KOReaderCredentialStore& store, const char* path, bool* needsResave) {
  return loadJsonWithBackup(
      "KRS", path, [&store](const String& json, bool* resave) { return loadKOReader(store, json.c_str(), resave); },
      needsResave);
}

// ---- WifiCredentialStore ----

bool JsonSettingsIO::saveWifi(const WifiCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["lastConnectedSsid"] = store.getLastConnectedSsid();

  JsonArray arr = doc["credentials"].to<JsonArray>();
  for (const auto& cred : store.getCredentials()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
  }

  String json;
  serializeJson(doc, json);
  return writeFileAtomic(path, json);
}

bool JsonSettingsIO::loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("WCS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.lastConnectedSsid = doc["lastConnectedSsid"] | std::string("");

  store.credentials.clear();
  JsonArray arr = doc["credentials"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.credentials.size() >= store.MAX_NETWORKS) break;
    WifiCredential cred;
    cred.ssid = obj["ssid"] | std::string("");
    bool ok = false;
    cred.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &ok);
    if (!ok || cred.password.empty()) {
      cred.password = obj["password"] | std::string("");
      if (!cred.password.empty() && needsResave) *needsResave = true;
    }
    store.credentials.push_back(cred);
  }

  LOG_DBG("WCS", "Loaded %zu WiFi credentials from file", store.credentials.size());
  return true;
}

bool JsonSettingsIO::loadWifiFile(WifiCredentialStore& store, const char* path, bool* needsResave) {
  return loadJsonWithBackup(
      "WCS", path, [&store](const String& json, bool* resave) { return loadWifi(store, json.c_str(), resave); },
      needsResave);
}

// ---- RecentBooksStore ----

bool JsonSettingsIO::saveRecentBooks(const RecentBooksStore& store, const char* path) {
  JsonDocument doc;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : store.getBooks()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
  }

  String json;
  serializeJson(doc, json);
  return writeFileAtomic(path, json);
}

bool JsonSettingsIO::loadRecentBooks(RecentBooksStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RBS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.recentBooks.clear();
  JsonArray arr = doc["books"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.getCount() >= 10) break;
    RecentBook book;
    book.path = obj["path"] | std::string("");
    book.title = obj["title"] | std::string("");
    book.author = obj["author"] | std::string("");
    book.coverBmpPath = obj["coverBmpPath"] | std::string("");
    store.recentBooks.push_back(book);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", store.getCount());
  return true;
}

bool JsonSettingsIO::loadRecentBooksFile(RecentBooksStore& store, const char* path) {
  return loadJsonWithBackup(
      "RBS", path, [&store](const String& json, bool*) { return loadRecentBooks(store, json.c_str()); }, nullptr);
}

// ---- OpdsServerStore ----
// Follows the same save/load pattern as WifiCredentialStore above.
// Passwords are XOR-obfuscated with the device MAC and base64-encoded ("password_obf" key).

bool JsonSettingsIO::saveOpds(const OpdsServerStore& store, const char* path) {
  JsonDocument doc;

  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : store.getServers()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["url"] = server.url;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
  }

  String json;
  serializeJson(doc, json);
  return writeFileAtomic(path, json);
}

bool JsonSettingsIO::loadOpds(OpdsServerStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("OPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.servers.clear();
  JsonArray arr = doc["servers"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.servers.size() >= OpdsServerStore::MAX_SERVERS) break;
    OpdsServer server;
    server.name = obj["name"] | std::string("");
    server.url = obj["url"] | std::string("");
    server.username = obj["username"] | std::string("");
    // Try the obfuscated key first; fall back to plaintext "password" for
    // files written before obfuscation was added (or hand-edited JSON).
    bool ok = false;
    server.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &ok);
    if (!ok || server.password.empty()) {
      server.password = obj["password"] | std::string("");
      if (!server.password.empty() && needsResave) *needsResave = true;
    }
    store.servers.push_back(std::move(server));
  }

  LOG_DBG("OPS", "Loaded %zu OPDS servers from file", store.servers.size());
  return true;
}

bool JsonSettingsIO::loadOpdsFile(OpdsServerStore& store, const char* path, bool* needsResave) {
  return loadJsonWithBackup(
      "OPS", path, [&store](const String& json, bool* resave) { return loadOpds(store, json.c_str(), resave); },
      needsResave);
}
