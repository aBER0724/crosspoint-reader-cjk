#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognized.
void clearBookCache(const std::string& path);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
