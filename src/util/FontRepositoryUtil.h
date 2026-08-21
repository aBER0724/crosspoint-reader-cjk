#pragma once

#include <string>

/**
 * Helpers for user-configured font repositories.
 *
 * A font repository is identified either by a GitHub "owner/repo" spec (the
 * device builds the manifest URL from that spec plus the current release
 * tag) or by a full HTTPS manifest URL (e.g. a self-hosted or Pages-hosted
 * catalog.json). These are pure functions so the firmware's unit tests can
 * exercise them directly.
 */

// The release tag used by the independent font repository's
// release-fonts.yml / build-fonts.yml workflows. Must match the tag the
// firmware's default FONT_MANIFEST_URL is published under.
#define FONT_RELEASE_TAG "sd-fonts-m2-b4"

// Validates a user-entered font repository spec. Two forms are accepted:
//  - GitHub "owner/repo": exactly one '/', non-empty owner and repo, no
//    whitespace / scheme / ".." / query / fragment / backslash / trailing '/'
//  - full HTTPS manifest URL: starts with "https://", contains no whitespace /
//    ".." / query / fragment / backslash, and its path ends in ".json"
//    (e.g. "https://example.com/fonts/catalog.json"). Non-TLS "http://" is
//    rejected.
bool isValidRepositorySpec(const std::string& spec);

// Assembles the fonts.json manifest URL for a GitHub repository, e.g.
//   "aBER0724/crosspoint-cjk-fonts" ->
//   "https://github.com/aBER0724/crosspoint-cjk-fonts/releases/download/sd-fonts-m2-b4/fonts.json"
// Callers must validate the spec with isValidRepositorySpec() first.
std::string assembleManifestUrl(const std::string& ownerRepo, const std::string& tag = FONT_RELEASE_TAG);

// Returns the manifest URL for any validated repository spec: full URLs are
// used as-is; "owner/repo" specs are routed through assembleManifestUrl().
std::string assembleRepositoryUrl(const std::string& spec, const std::string& tag = FONT_RELEASE_TAG);
