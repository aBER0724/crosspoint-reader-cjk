#include "StoragePathPolicy.h"

#include <cctype>
#include <cstddef>

namespace {
const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);

bool equalsIgnoreCase(const char* left, const char* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  while (*left != '\0' && *right != '\0') {
    const unsigned char leftChar = static_cast<unsigned char>(*left);
    const unsigned char rightChar = static_cast<unsigned char>(*right);
    if (std::tolower(leftChar) != std::tolower(rightChar)) {
      return false;
    }
    left++;
    right++;
  }
  return *left == '\0' && *right == '\0';
}
}  // namespace

namespace StoragePathPolicy {

bool isProtectedItemName(const char* name) {
  if (name == nullptr || name[0] == '\0') {
    return false;
  }
  if (name[0] == '.') {
    return true;
  }
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (equalsIgnoreCase(name, HIDDEN_ITEMS[i])) {
      return true;
    }
  }
  return false;
}

bool isProtectedItemName(const String& name) { return isProtectedItemName(name.c_str()); }

bool isProtectedPath(const String& path) {
  int start = 0;
  while (start < (int)path.length()) {
    if (path.charAt(start) == '/') {
      start++;
      continue;
    }
    int end = path.indexOf('/', start);
    if (end == -1) {
      end = path.length();
    }

    const String segment = path.substring(start, end);
    if (isProtectedItemName(segment)) {
      return true;
    }

    start = end + 1;
  }

  return false;
}

}  // namespace StoragePathPolicy
