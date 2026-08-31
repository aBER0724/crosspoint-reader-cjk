#pragma once

#include <string>

/**
 * Helpers for user-configured font repositories ("owner/repo").
 *
 * A font repository is identified by its GitHub "owner/repo" spec. The device
 * builds the manifest URL from that spec plus the current release tag. These
 * are pure functions so the firmware's unit tests can exercise them directly.
 */

// Validates a user-entered GitHub "owner/repo" spec:
//  - non-empty after trimming
//  - exactly one '/'
//  - owner and repo are both non-empty
//  - no whitespace, no scheme ("://"), no "..", no trailing slash
//  - no query/fragment/backslash characters
bool isValidRepositorySpec(const std::string& spec);

// Assembles the GitHub Contents API URL for fonts.json on the repository's
// main branch. Callers must validate the spec first.
std::string assembleManifestUrl(const std::string& ownerRepo);
