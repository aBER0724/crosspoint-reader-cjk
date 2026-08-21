#include "util/FontRepositoryUtil.h"

#include <algorithm>

namespace {

std::string trim(std::string v) {
  const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!v.empty() && isSpace(v.front())) v.erase(v.begin());
  while (!v.empty() && isSpace(v.back())) v.pop_back();
  return v;
}

// Common lexical checks shared by both spec forms: no embedded whitespace, no
// scheme separators beyond a leading "https://", no path traversal, no query /
// fragment / backslash characters.
bool hasForbiddenCharacters(const std::string& s) {
  if (s.find("..") != std::string::npos) return true;
  if (s.find_first_of(" \t\r\n\\?#") != std::string::npos) return true;
  return false;
}

// True when the string starts with the given lowercase prefix.
bool startsWith(const std::string& s, const char* prefix) {
  const size_t n = std::string(prefix).size();
  return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// Full HTTPS manifest URL form: "https://host/path/.../fonts.json".
bool isValidManifestUrl(const std::string& s) {
  if (!startsWith(s, "https://")) return false;                  // TLS only
  if (s.size() <= std::string("https://").size()) return false;  // host missing
  if (hasForbiddenCharacters(s)) return false;
  // The path must end with a JSON manifest file.
  if (s.size() < 5 || s.compare(s.size() - 5, 5, ".json") != 0) return false;
  return true;
}

// GitHub "owner/repo" form.
bool isValidOwnerRepo(const std::string& s) {
  if (s.find("://") != std::string::npos) return false;
  if (hasForbiddenCharacters(s)) return false;

  // Exactly one '/', with a non-empty owner and a non-empty repo.
  const size_t first = s.find('/');
  if (first == std::string::npos) return false;
  const size_t last = s.rfind('/');
  if (first != last) return false;
  if (first == 0) return false;
  if (last == s.size() - 1) return false;

  return true;
}

}  // namespace

bool isValidRepositorySpec(const std::string& spec) {
  const std::string s = trim(spec);
  if (s.empty()) return false;

  // A full HTTPS manifest URL, or a GitHub owner/repo spec.
  return isValidManifestUrl(s) || isValidOwnerRepo(s);
}

std::string assembleManifestUrl(const std::string& ownerRepo, const std::string& tag) {
  return "https://github.com/" + ownerRepo + "/releases/download/" + tag + "/fonts.json";
}

std::string assembleRepositoryUrl(const std::string& spec, const std::string& tag) {
  const std::string s = trim(spec);
  if (startsWith(s, "https://")) {
    return s;  // full manifest URL: use as-is
  }
  return assembleManifestUrl(s, tag);
}
