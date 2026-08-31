#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * Shared font-manifest types and pure merge helpers.
 *
 * A fonts.json manifest describes one repository's font catalog: a baseUrl
 * (where the .cpfont files live) plus a list of families, each with a list of
 * point-size files. The firmware merges several repositories' manifests into a
 * single family list, deduplicating by family name + point size so that a fork
 * of the default repository contributes only its unique fonts.
 */

// One .cpfont entry from a fonts.json manifest. baseUrl records which
// repository the file is downloaded from (a family may mix point sizes from
// several repositories after merging).
struct ManifestFile {
  std::shared_ptr<const std::string> baseUrl;
  size_t size = 0;
  uint8_t pointSize = 0;
  std::array<uint8_t, 32> sha256{};
  uint32_t fingerprint = 0;
  bool installed = false;
};

// A font family (one display name) with its point-size files.
struct ManifestFamily {
  std::string name;
  std::string description;
  std::vector<ManifestFile> files;
  size_t totalSize = 0;
  uint32_t fingerprint = 0;
  bool installed = false;
  bool partial = false;
  bool hasUpdate = false;
};

// Canonical on-card / release-asset file name: "<Family>_<size>.cpfont".
std::string manifestFileName(const ManifestFamily& family, const ManifestFile& file);

// FNV-1a fingerprint over the family's file set (name + size + sha256 per
// file). Used to detect whether the installed family matches the catalog.
uint32_t computeFamilyFingerprint(const ManifestFamily& family);

// Merges families from a later (lower-priority) repository into `out`.
// Earlier repositories take priority: an existing family name is kept, and
// within it an existing point size is kept. Point sizes that are still
// missing are appended from the incoming repository. Recomputes totalSize and
// fingerprint on merged families. Returns the number of families appended.
size_t mergeManifestFamilies(std::vector<ManifestFamily>& out, std::vector<ManifestFamily>&& incoming);
