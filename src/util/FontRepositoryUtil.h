#pragma once

#include <string>

/**
 * Helpers for user-configured font repositories.
 *
 * A repository is identified either by a GitHub "owner/repo" spec or by a
 * direct HTTPS URL to a JSON manifest. These pure helpers are shared by the
 * firmware and host-side tests.
 */

// Accepts a GitHub "owner/repo" or a direct HTTPS manifest URL. Direct URLs
// must have a non-empty host and path ending in ".json". HTTP, credentials,
// whitespace, traversal, query, fragment, and backslash forms are rejected.
bool isValidRepositorySpec(const std::string& spec);

// Builds the current GitHub Contents API manifest URL for an owner/repo spec.
std::string assembleManifestUrl(const std::string& ownerRepo);

// Returns a trimmed direct HTTPS manifest URL, or maps owner/repo through
// assembleManifestUrl(). Callers must validate the spec first.
std::string assembleRepositoryUrl(const std::string& spec);
