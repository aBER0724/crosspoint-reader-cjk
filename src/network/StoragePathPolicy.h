#pragma once

#include <WString.h>

namespace StoragePathPolicy {

bool isProtectedItemName(const char* name);
bool isProtectedItemName(const String& name);
bool isProtectedPath(const String& path);

}  // namespace StoragePathPolicy
