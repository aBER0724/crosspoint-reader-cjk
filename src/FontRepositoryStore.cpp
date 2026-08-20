#include "FontRepositoryStore.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

void FontRepositoryStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["repositories"].to<JsonArray>();
  for (const auto& repo : repositories) {
    arr.add(repo);
  }
}

bool FontRepositoryStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'repositories' key (treat as empty list); only
  // a JSON parse error is fatal. A null JsonArray iterates zero times.
  repositories.clear();
  JsonArrayConst arr = doc["repositories"].as<JsonArrayConst>();
  repositories.reserve(std::min(arr.size(), MAX_REPOSITORIES));

  for (JsonVariantConst v : arr) {
    if (repositories.size() >= FontRepositoryStore::MAX_REPOSITORIES) break;
    const char* spec = v | "";
    if (spec == nullptr || spec[0] == '\0') continue;
    repositories.emplace_back(spec);
  }

  LOG_DBG("FRP", "Loaded %zu font repositories from file", repositories.size());
  return true;
}

bool FontRepositoryStore::addRepository(const std::string& spec) {
  if (repositories.size() >= MAX_REPOSITORIES) {
    LOG_DBG("FRP", "Cannot add more repositories, limit of %zu reached", MAX_REPOSITORIES);
    return false;
  }

  repositories.push_back(spec);
  LOG_DBG("FRP", "Added repository: %s", spec.c_str());
  return saveToFile();
}

bool FontRepositoryStore::updateRepository(size_t index, const std::string& spec) {
  if (index >= repositories.size()) {
    return false;
  }

  repositories[index] = spec;
  LOG_DBG("FRP", "Updated repository %zu: %s", index, spec.c_str());
  return saveToFile();
}

bool FontRepositoryStore::removeRepository(size_t index) {
  if (index >= repositories.size()) {
    return false;
  }

  LOG_DBG("FRP", "Removed repository: %s", repositories[index].c_str());
  repositories.erase(repositories.begin() + static_cast<ptrdiff_t>(index));
  return saveToFile();
}

const std::string* FontRepositoryStore::getRepository(size_t index) const {
  if (index >= repositories.size()) {
    return nullptr;
  }
  return &repositories[index];
}
