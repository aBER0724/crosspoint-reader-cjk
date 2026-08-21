#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

/**
 * Singleton storing the user's configured font repositories on the SD card.
 *
 * Each entry is a GitHub "owner/repo" spec (e.g. "aBER0724/crosspoint-cjk-fonts").
 * The device downloads and merges manifests from these repositories in
 * addition to the built-in default, deduplicating by family name + point size.
 */
class FontRepositoryStore : public PersistableStore<FontRepositoryStore> {
 private:
  std::vector<std::string> repositories;

  static constexpr size_t MAX_REPOSITORIES = 8;

  FontRepositoryStore() = default;

  friend class PersistableStore<FontRepositoryStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/font-repos.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool addRepository(const std::string& spec);
  bool updateRepository(size_t index, const std::string& spec);
  bool removeRepository(size_t index);

  const std::vector<std::string>& getRepositories() const { return repositories; }
  const std::string* getRepository(size_t index) const;
  size_t getCount() const { return repositories.size(); }
  bool hasRepositories() const { return !repositories.empty(); }
};

#define FONT_REPO_STORE FontRepositoryStore::getInstance()
