#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "util/FontManifest.h"
#include "util/FontRepositoryUtil.h"

namespace {

// Builds a single-file family with the given point size / size / sha seed.
ManifestFamily makeFamily(const std::string& name, uint8_t pointSize, size_t size = 100, uint8_t shaSeed = 0) {
  ManifestFamily family;
  family.name = name;
  family.description = name + " description";
  ManifestFile file;
  file.pointSize = pointSize;
  file.size = size;
  for (size_t i = 0; i < file.sha256.size(); ++i) {
    file.sha256[i] = static_cast<uint8_t>(shaSeed + i);
  }
  family.files.push_back(file);
  family.totalSize = size;
  return family;
}

}  // namespace

// ---------------------------------------------------------------------------
// Repository spec validation
// ---------------------------------------------------------------------------

TEST(FontRepositoryUtilTest, ValidSpecs) {
  // GitHub owner/repo form.
  EXPECT_TRUE(isValidRepositorySpec("aBER0724/crosspoint-cjk-fonts"));
  EXPECT_TRUE(isValidRepositorySpec("user/repo"));
  EXPECT_TRUE(isValidRepositorySpec("a-b/c_d"));
  EXPECT_TRUE(isValidRepositorySpec("  user/repo  "));  // surrounding whitespace trimmed

  // Full HTTPS manifest URL form.
  EXPECT_TRUE(isValidRepositorySpec("https://github.com/user/repo/releases/download/sd-fonts-m2-b4/fonts.json"));
  EXPECT_TRUE(isValidRepositorySpec("https://example.com/fonts/catalog.json"));
  EXPECT_TRUE(isValidRepositorySpec("https://raw.githubusercontent.com/a/b/main/fonts.json"));
  EXPECT_TRUE(isValidRepositorySpec("  https://example.com/fonts.json  "));  // trimmed
}

TEST(FontRepositoryUtilTest, InvalidSpecs) {
  EXPECT_FALSE(isValidRepositorySpec(""));
  EXPECT_FALSE(isValidRepositorySpec("   "));
  EXPECT_FALSE(isValidRepositorySpec("owner"));
  EXPECT_FALSE(isValidRepositorySpec("owner/"));
  EXPECT_FALSE(isValidRepositorySpec("/repo"));
  EXPECT_FALSE(isValidRepositorySpec("owner/repo/extra"));
  EXPECT_FALSE(isValidRepositorySpec("user/../repo"));
  EXPECT_FALSE(isValidRepositorySpec("user/repo?x=1"));
  EXPECT_FALSE(isValidRepositorySpec("user/repo#frag"));
  EXPECT_FALSE(isValidRepositorySpec("user repo/x"));
  EXPECT_FALSE(isValidRepositorySpec("user\\repo"));

  // Non-TLS http:// URLs are rejected (manifest downloads require TLS).
  EXPECT_FALSE(isValidRepositorySpec("http://github.com/user/repo"));
  EXPECT_FALSE(isValidRepositorySpec("http://example.com/fonts.json"));

  // Malformed / non-manifest URLs are rejected.
  EXPECT_FALSE(isValidRepositorySpec("https://x/y"));  // path does not end in .json
  EXPECT_FALSE(isValidRepositorySpec("https://example.com/fonts.txt"));
  EXPECT_FALSE(isValidRepositorySpec("https://example.com"));                     // no path
  EXPECT_FALSE(isValidRepositorySpec("https://"));                                // no host
  EXPECT_FALSE(isValidRepositorySpec("https://example.com/fonts.json?x=1"));      // query rejected
  EXPECT_FALSE(isValidRepositorySpec("https://example.com/fonts.json#frag"));     // fragment rejected
  EXPECT_FALSE(isValidRepositorySpec("https://example.com/a/../fonts.json"));     // traversal rejected
  EXPECT_FALSE(isValidRepositorySpec("https://example.com/foo bar/fonts.json"));  // whitespace rejected
}

TEST(FontRepositoryUtilTest, ReleaseTagConstant) { EXPECT_STREQ(FONT_RELEASE_TAG, "sd-fonts-m2-b4"); }

TEST(FontRepositoryUtilTest, AssembleManifestUrl) {
  EXPECT_EQ(assembleManifestUrl("user/repo", FONT_RELEASE_TAG),
            "https://github.com/user/repo/releases/download/sd-fonts-m2-b4/fonts.json");
  EXPECT_EQ(assembleManifestUrl("aBER0724/crosspoint-cjk-fonts", FONT_RELEASE_TAG),
            "https://github.com/aBER0724/crosspoint-cjk-fonts/releases/download/sd-fonts-m2-b4/fonts.json");
  EXPECT_EQ(assembleManifestUrl("user/repo", "v1.0"), "https://github.com/user/repo/releases/download/v1.0/fonts.json");
}

TEST(FontRepositoryUtilTest, AssembleRepositoryUrl) {
  // Owner/repo form routes through assembleManifestUrl with the default tag.
  EXPECT_EQ(assembleRepositoryUrl("user/repo"),
            "https://github.com/user/repo/releases/download/sd-fonts-m2-b4/fonts.json");
  EXPECT_EQ(assembleRepositoryUrl("  user/repo  "),
            "https://github.com/user/repo/releases/download/sd-fonts-m2-b4/fonts.json");
  EXPECT_EQ(assembleRepositoryUrl("user/repo", "v1.0"),
            "https://github.com/user/repo/releases/download/v1.0/fonts.json");

  // Full HTTPS manifest URL form is used verbatim (tag is ignored).
  const std::string kCatalogUrl = "https://example.com/fonts/catalog.json";
  EXPECT_EQ(assembleRepositoryUrl(kCatalogUrl), kCatalogUrl);
  EXPECT_EQ(assembleRepositoryUrl(kCatalogUrl, "v1.0"), kCatalogUrl);
  const std::string kGhUrl = "https://github.com/owner/repo/releases/download/custom-tag/fonts.json";
  EXPECT_EQ(assembleRepositoryUrl(kGhUrl), kGhUrl);
}

// ---------------------------------------------------------------------------
// manifestFileName / fingerprint
// ---------------------------------------------------------------------------

TEST(FontManifestTest, ManifestFileName) {
  auto family = makeFamily("NotoSansSC", 14);
  EXPECT_EQ(manifestFileName(family, family.files[0]), "NotoSansSC_14.cpfont");

  auto family2 = makeFamily("BrightReadingTC", 22);
  EXPECT_EQ(manifestFileName(family2, family2.files[0]), "BrightReadingTC_22.cpfont");
}

TEST(FontManifestTest, FingerprintDeterministic) {
  auto a = makeFamily("Family", 14);
  auto b = makeFamily("Family", 14);
  EXPECT_EQ(computeFamilyFingerprint(a), computeFamilyFingerprint(b));
}

TEST(FontManifestTest, FingerprintDiffers) {
  auto a = makeFamily("Family", 14, 100, 1);
  auto b = makeFamily("Family", 18, 100, 1);
  EXPECT_NE(computeFamilyFingerprint(a), computeFamilyFingerprint(b));

  auto c = makeFamily("Family", 14, 200, 1);
  EXPECT_NE(computeFamilyFingerprint(a), computeFamilyFingerprint(c));
}

// ---------------------------------------------------------------------------
// Manifest merging / deduplication
// ---------------------------------------------------------------------------

TEST(FontManifestMergeTest, DistinctFamiliesAppended) {
  std::vector<ManifestFamily> out;
  out.push_back(makeFamily("FamilyA", 14));

  std::vector<ManifestFamily> incoming;
  incoming.push_back(makeFamily("FamilyB", 16));

  const size_t added = mergeManifestFamilies(out, std::move(incoming));
  EXPECT_EQ(added, 1u);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].name, "FamilyA");
  EXPECT_EQ(out[1].name, "FamilyB");
}

TEST(FontManifestMergeTest, DuplicateFamilyPointSizeDeduped) {
  auto base = makeFamily("Family", 14, 100, 1);
  std::vector<ManifestFamily> out;
  out.push_back(base);

  // Incoming fork declares the same family with the same 14 pt file but from a
  // different repository (different baseUrl). Earlier repo wins.
  auto fork = makeFamily("Family", 14, 9999, 9);
  fork.files[0].baseUrl = "https://github.com/fork/repo/releases/download/x";
  std::vector<ManifestFamily> incoming;
  incoming.push_back(std::move(fork));

  const size_t added = mergeManifestFamilies(out, std::move(incoming));
  EXPECT_EQ(added, 0u);
  ASSERT_EQ(out.size(), 1u);
  ASSERT_EQ(out[0].files.size(), 1u);
  EXPECT_EQ(out[0].files[0].size, 100u);  // original kept, fork ignored
  EXPECT_TRUE(out[0].files[0].baseUrl.empty());
  EXPECT_EQ(out[0].totalSize, 100u);
}

TEST(FontManifestMergeTest, MissingPointSizeAppendedFromFork) {
  auto base = makeFamily("Family", 14, 100, 1);
  std::vector<ManifestFamily> out;
  out.push_back(base);

  // Fork provides an extra 18 pt file that the default repo does not ship.
  auto fork = makeFamily("Family", 18, 200, 2);
  fork.files[0].baseUrl = "https://github.com/fork/repo/releases/download/x";
  std::vector<ManifestFamily> incoming;
  incoming.push_back(std::move(fork));

  const size_t added = mergeManifestFamilies(out, std::move(incoming));
  EXPECT_EQ(added, 0u);  // family already existed
  ASSERT_EQ(out.size(), 1u);
  ASSERT_EQ(out[0].files.size(), 2u);

  // Sorted by point size: 14 (default) then 18 (fork, with its own baseUrl).
  EXPECT_EQ(out[0].files[0].pointSize, 14);
  EXPECT_EQ(out[0].files[1].pointSize, 18);
  EXPECT_EQ(out[0].files[1].baseUrl, "https://github.com/fork/repo/releases/download/x");

  // totalSize recomputed to include the appended file.
  EXPECT_EQ(out[0].totalSize, 300u);

  // Fingerprint recomputed over the merged set.
  EXPECT_EQ(out[0].fingerprint, computeFamilyFingerprint(out[0]));
}

TEST(FontManifestMergeTest, MultiRepoOrderAndPriority) {
  std::vector<ManifestFamily> out;
  // Default repo: FamilyA 14pt, FamilyB 14pt.
  out.push_back(makeFamily("FamilyA", 14));
  out.push_back(makeFamily("FamilyB", 14));

  // First custom repo: FamilyA 18pt (new size for A), FamilyB 14pt (dup).
  std::vector<ManifestFamily> repo1;
  repo1.push_back(makeFamily("FamilyA", 18, 150, 3));
  repo1.push_back(makeFamily("FamilyB", 14, 999, 9));
  EXPECT_EQ(mergeManifestFamilies(out, std::move(repo1)), 0u);

  // Second custom repo: FamilyC 20pt (new family).
  std::vector<ManifestFamily> repo2;
  repo2.push_back(makeFamily("FamilyC", 20));
  EXPECT_EQ(mergeManifestFamilies(out, std::move(repo2)), 1u);

  ASSERT_EQ(out.size(), 3u);
  ASSERT_EQ(out[0].files.size(), 2u);
  EXPECT_EQ(out[0].files[0].pointSize, 14);
  EXPECT_EQ(out[0].files[1].pointSize, 18);
  EXPECT_EQ(out[1].files.size(), 1u);  // FamilyB dup dropped
  EXPECT_EQ(out[2].name, "FamilyC");
}

TEST(FontManifestMergeTest, DefaultRepoPriorityOverCustom) {
  std::vector<ManifestFamily> out;
  auto defaultFamily = makeFamily("Shared", 14, 100, 1);
  out.push_back(defaultFamily);

  // A fork re-ships Shared 14pt with a different sha; the default must win.
  std::vector<ManifestFamily> fork;
  fork.push_back(makeFamily("Shared", 14, 999, 7));
  mergeManifestFamilies(out, std::move(fork));

  ASSERT_EQ(out[0].files.size(), 1u);
  EXPECT_EQ(out[0].files[0].size, 100u);
  EXPECT_EQ(out[0].files[0].sha256[0], 1u);
}
