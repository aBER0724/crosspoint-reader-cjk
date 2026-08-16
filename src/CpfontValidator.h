#pragma once

#include <cstddef>

namespace CpfontValidator {

using ReadAt = int (*)(void* context, size_t offset, void* destination, size_t length);

struct Reader {
  void* context;
  size_t fileSize;
  ReadAt readAt;
};

/// Validate a canonical CPFONT v4 file without allocating file-sized memory.
/// This is intended for font installation and download acceptance paths only.
bool validateV4(const Reader& reader);

}  // namespace CpfontValidator
