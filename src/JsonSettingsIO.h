#pragma once

class CrossPointSettings;
class CrossPointState;
class WifiCredentialStore;
class KOReaderCredentialStore;
class RecentBooksStore;
class OpdsServerStore;

namespace JsonSettingsIO {

bool jsonFileOrBackupExists(const char* path);

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);
bool loadSettingsFile(CrossPointSettings& s, const char* path, bool* needsResave = nullptr);

// CrossPointState
bool saveState(const CrossPointState& s, const char* path);
bool loadState(CrossPointState& s, const char* json);
bool loadStateFile(CrossPointState& s, const char* path);

// WifiCredentialStore
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave = nullptr);
bool loadWifiFile(WifiCredentialStore& store, const char* path, bool* needsResave = nullptr);

// KOReaderCredentialStore
bool saveKOReader(const KOReaderCredentialStore& store, const char* path);
bool loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave = nullptr);
bool loadKOReaderFile(KOReaderCredentialStore& store, const char* path, bool* needsResave = nullptr);

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);
bool loadRecentBooksFile(RecentBooksStore& store, const char* path);

// OpdsServerStore
bool saveOpds(const OpdsServerStore& store, const char* path);
bool loadOpds(OpdsServerStore& store, const char* json, bool* needsResave = nullptr);
bool loadOpdsFile(OpdsServerStore& store, const char* path, bool* needsResave = nullptr);

}  // namespace JsonSettingsIO
