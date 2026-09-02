#include "util/FontRepositoryUtil.h"

#include <algorithm>

namespace {

std::string trim(std::string value) {
  const auto isSpace = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!value.empty() && isSpace(value.front())) value.erase(value.begin());
  while (!value.empty() && isSpace(value.back())) value.pop_back();
  return value;
}

bool hasForbiddenCharacters(const std::string& value) {
  return value.find("..") != std::string::npos || value.find_first_of(" \t\r\n\\?#") != std::string::npos;
}

bool isValidManifestUrl(const std::string& value) {
  constexpr const char* HTTPS_PREFIX = "https://";
  constexpr size_t HTTPS_PREFIX_LENGTH = 8;
  if (value.compare(0, HTTPS_PREFIX_LENGTH, HTTPS_PREFIX) != 0 || hasForbiddenCharacters(value)) return false;

  const size_t pathStart = value.find('/', HTTPS_PREFIX_LENGTH);
  if (pathStart == std::string::npos || pathStart == HTTPS_PREFIX_LENGTH) return false;

  const std::string authority = value.substr(HTTPS_PREFIX_LENGTH, pathStart - HTTPS_PREFIX_LENGTH);
  if (authority.find('@') != std::string::npos) return false;

  constexpr const char* JSON_SUFFIX = ".json";
  constexpr size_t JSON_SUFFIX_LENGTH = 5;
  return value.size() >= pathStart + JSON_SUFFIX_LENGTH &&
         value.compare(value.size() - JSON_SUFFIX_LENGTH, JSON_SUFFIX_LENGTH, JSON_SUFFIX) == 0;
}

bool isValidOwnerRepo(const std::string& value) {
  if (value.find("://") != std::string::npos || hasForbiddenCharacters(value)) return false;

  const size_t first = value.find('/');
  return first != std::string::npos && first == value.rfind('/') && first > 0 && first + 1 < value.size();
}

}  // namespace

bool isValidRepositorySpec(const std::string& spec) {
  const std::string value = trim(spec);
  return !value.empty() && (isValidManifestUrl(value) || isValidOwnerRepo(value));
}

std::string assembleManifestUrl(const std::string& ownerRepo) {
  return "https://api.github.com/repos/" + ownerRepo + "/contents/fonts.json?ref=main";
}

std::string assembleRepositoryUrl(const std::string& spec) {
  const std::string value = trim(spec);
  return value.compare(0, 8, "https://") == 0 ? value : assembleManifestUrl(value);
}
