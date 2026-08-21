#pragma once

#include <string>

/**
 * Helpers for user-configured font repositories ("owner/repo").
 *
 * A font repository is identified by its GitHub "owner/repo" spec. The device
 * builds the manifest URL from that spec plus the current release tag. These
 * are pure functions so the firmware's unit tests can exercise them directly.
 */

// The release tag used by the independent font repository's
// release-fonts.yml / build-fonts.yml workflows. Must match the tag the
// firmware's default FONT_MANIFEST_URL is published under.
#define FONT_RELEASE_TAG "sd-fonts-m2-b4"

// Validates a user-entered GitHub "owner/repo" spec:
//  - non-empty after trimming
//  - exactly one '/'
//  - owner and repo are both non-empty
//  - no whitespace, no scheme ("://"), no "..", no trailing slash
//  - no query/fragment/backslash characters
bool isValidRepositorySpec(const std::string& spec);

// Assembles the fonts.json manifest URL for a repository, e.g.
//   "aBER0724/crosspoint-cjk-fonts" ->
//   "https://github.com/aBER0724/crosspoint-cjk-fonts/releases/download/sd-fonts-m2-b4/fonts.json"
// Callers must validate the spec with isValidRepositorySpec() first.
std::string assembleManifestUrl(const std::string& ownerRepo, const std::string& tag = FONT_RELEASE_TAG);
