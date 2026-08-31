#include "util/FontRepositoryUtil.h"

#include <algorithm>

namespace {

std::string trim(std::string v) {
  const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!v.empty() && isSpace(v.front())) v.erase(v.begin());
  while (!v.empty() && isSpace(v.back())) v.pop_back();
  return v;
}

}  // namespace

bool isValidRepositorySpec(const std::string& spec) {
  const std::string s = trim(spec);
  if (s.empty()) return false;

  // Reject scheme separators, path traversal, backslashes, and URL syntax.
  if (s.find("://") != std::string::npos) return false;
  if (s.find("..") != std::string::npos) return false;
  if (s.find_first_of(" \t\r\n\\?#") != std::string::npos) return false;

  // Exactly one '/', with a non-empty owner and a non-empty repo.
  const size_t first = s.find('/');
  if (first == std::string::npos) return false;
  const size_t last = s.rfind('/');
  if (first != last) return false;
  if (first == 0) return false;
  if (last == s.size() - 1) return false;

  return true;
}

std::string assembleManifestUrl(const std::string& ownerRepo) {
  return "https://api.github.com/repos/" + ownerRepo + "/contents/fonts.json?ref=main";
}
