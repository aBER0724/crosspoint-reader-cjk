#pragma once

#include <SdCardFontRegistry.h>

#include <cstddef>
#include <cstdint>

/// Shared utility for font installation (device download + browser upload).
/// Handles directory creation, file validation, deletion, and registry refresh.
class FontInstaller {
 public:
  static constexpr size_t MAX_FAMILY_NAME_LENGTH = 48;
  static constexpr size_t MAX_CPFONT_FILENAME_LENGTH = 80;

  enum class Error {
    OK,
    INVALID_FAMILY_NAME,
    INVALID_FILE,
    SD_WRITE_ERROR,
    MAX_FAMILIES_REACHED,
  };

  explicit FontInstaller(SdCardFontRegistry& registry);

  /// Validate a family name: alphanumeric + hyphen + underscore only, no path traversal.
  static bool isValidFamilyName(const char* name);

  /// Validate a .cpfont filename: ends with ".cpfont", no path separators or
  /// traversal sequences, and the basename uses only alphanumeric + hyphen +
  /// underscore. Rejects "../foo.cpfont" and "evil/foo.cpfont".
  static bool isValidCpfontFilename(const char* name);

  /// Recover interrupted transactional replacements under both font roots.
  /// Family transactions are rolled back unless their committed marker is
  /// present. Legacy per-file .part/.bak transactions are also recovered.
  static bool recoverInterruptedInstalls();

  /// Start a durable family-level transaction. Must be called before any
  /// installed file is moved or replaced.
  static bool beginFamilyInstall(const char* familyName, uint32_t fingerprint);

  /// Preserve the current file for rollback, or record that the replacement
  /// is a newly installed file when no current file exists.
  static bool prepareFontReplacement(const char* finalPath);

  /// Atomically switch a family transaction from rollback to committed state.
  static bool commitFamilyInstall(const char* familyName);

  /// Roll back every replacement in the family and remove transaction files.
  static bool rollbackFamilyInstall(const char* familyName);

  /// Remove backups and transaction markers after a successful commit.
  static bool cleanupCommittedFamilyInstall(const char* familyName);

  /// Compare the installed family receipt with the current manifest metadata.
  /// This reads only a small receipt and never scans the font files.
  static bool installedFamilyMatches(const char* familyName, uint32_t fingerprint);

  /// Ensure /<root>/<family>/ exists, where <root> is /.fonts (preferred) or /fonts.
  /// Re-uses the existing root if the family is already installed; otherwise
  /// creates it under SdCardFontRegistry::defaultWriteRoot().
  bool ensureFamilyDir(const char* familyName);

  /// Validate a .cpfont file on disk (check magic bytes).
  bool validateCpfontFile(const char* path);

  /// Build the full SD path for a font file.
  /// Writes "/<root>/<family>/<filename>" to outBuf, choosing <root> the same
  /// way ensureFamilyDir does (existing install dir, else default-write root).
  /// Returns false when either name is invalid or the path does not fit.
  static bool buildFontPath(const char* family, const char* filename, char* outBuf, size_t outBufSize);

  /// Delete a family directory and all .cpfont files in it.
  /// If the deleted family is the active reader font, clears the setting.
  Error deleteFamily(const char* familyName);

  /// Re-run registry discovery to pick up new/removed fonts.
  void refreshRegistry();

  /// Check whether a family name already exists in the registry.
  bool isFamilyInstalled(const char* familyName) const;

 private:
  SdCardFontRegistry& registry_;

  static constexpr const char* CPFONT_MAGIC = "CPFONT\0";
  static constexpr size_t CPFONT_MAGIC_LEN = 8;
};
