#include "util/FontManifest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {

// FNV-1a rolling hash helper. A zero-length key is a valid intermediate state
// (used when fingerprinting structured fields byte by byte).
uint32_t fingerprintBytes(const void* data, size_t len, uint32_t hash) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; i++) {
    hash ^= bytes[i];
    hash *= 16777619U;
  }
  return hash;
}

uint32_t fingerprintString(const std::string& s, uint32_t hash) { return fingerprintBytes(s.data(), s.size(), hash); }

}  // namespace

std::string manifestFileName(const ManifestFamily& family, const ManifestFile& file) {
  return family.name + "_" + std::to_string(file.pointSize) + ".cpfont";
}

uint32_t computeFamilyFingerprint(const ManifestFamily& family) {
  uint32_t fingerprint = 2166136261U;
  for (const auto& file : family.files) {
    const std::string fileName = manifestFileName(family, file);
    fingerprint = fingerprintString(fileName, fingerprint);
    fingerprint = fingerprintBytes(&file.size, sizeof(file.size), fingerprint);
    fingerprint = fingerprintBytes(file.sha256.data(), file.sha256.size(), fingerprint);
  }
  return fingerprint;
}

size_t mergeManifestFamilies(std::vector<ManifestFamily>& out, std::vector<ManifestFamily>&& incoming) {
  size_t added = 0;
  for (auto& inFamily : incoming) {
    auto it =
        std::find_if(out.begin(), out.end(), [&inFamily](const ManifestFamily& f) { return f.name == inFamily.name; });
    if (it == out.end()) {
      out.push_back(std::move(inFamily));
      ++added;
      continue;
    }

    // Same family already present from an earlier repository: append only the
    // point sizes that are still missing. Existing entries (and their source
    // baseUrl) are left untouched.
    bool appended = false;
    for (auto& inFile : inFamily.files) {
      const bool hasSize = std::any_of(it->files.begin(), it->files.end(),
                                       [&inFile](const ManifestFile& f) { return f.pointSize == inFile.pointSize; });
      if (!hasSize) {
        it->files.push_back(std::move(inFile));
        appended = true;
      }
    }
    if (!appended) continue;

    std::sort(it->files.begin(), it->files.end(),
              [](const ManifestFile& a, const ManifestFile& b) { return a.pointSize < b.pointSize; });
    it->totalSize = 0;
    for (const auto& f : it->files) {
      it->totalSize += f.size;
    }
    it->fingerprint = computeFamilyFingerprint(*it);
  }
  return added;
}
