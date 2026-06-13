#include "ParsedText.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

// Em-space (U+2003) UTF-8 bytes; used as a fallback paragraph indent.
constexpr char EM_SPACE_UTF8[] = "\xe2\x80\x83";
constexpr size_t EM_SPACE_BYTES = 3;

// Words coming out of the parser are bounded by ChapterHtmlSlimParser's
// MAX_WORD_SIZE = 200. With paragraph indent (+3) or hyphenation (+1) the
// worst case is ~204 bytes; 256 leaves comfortable headroom plus the
// trailing null terminator that C-string APIs expect.
constexpr size_t WORD_NULL_TERM_BUF = 256;

// Copies `word` into `buf` and null-terminates it. Returns the buffer for
// chaining into a C-string API. If the word is larger than the buffer we
// truncate to keep the call infallible — this should never happen given
// MAX_WORD_SIZE but the bound is enforced defensively.
const char* nullTerminate(char (&buf)[WORD_NULL_TERM_BUF], std::string_view word) {
  const size_t copyLen = std::min(word.size(), WORD_NULL_TERM_BUF - 1);
  if (copyLen > 0) {
    std::memcpy(buf, word.data(), copyLen);
  }
  buf[copyLen] = '\0';
  return buf;
}

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(std::string_view word) {
  if (word.empty()) return 0;
  char buf[WORD_NULL_TERM_BUF];
  nullTerminate(buf, word);
  const auto* ptr = reinterpret_cast<const unsigned char*>(buf);
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(std::string_view word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  // Copy the last UTF-8 sequence into a small null-terminated buffer.
  char buf[8];
  const size_t copyLen = std::min(word.size() - i, sizeof(buf) - 1);
  std::memcpy(buf, word.data() + i, copyLen);
  buf[copyLen] = '\0';
  const auto* ptr = reinterpret_cast<const unsigned char*>(buf);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(std::string_view word) {
  return word.find(std::string_view(SOFT_HYPHEN_UTF8, SOFT_HYPHEN_BYTES)) != std::string_view::npos;
}

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos, SOFT_HYPHEN_BYTES)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Builds a sanitized copy of `word` (soft hyphens stripped, optional trailing
// hyphen appended) into `outBuf`. Returns the resulting C-string.
const char* sanitizeWord(char (&outBuf)[WORD_NULL_TERM_BUF], std::string_view word, bool appendHyphen) {
  size_t outLen = 0;
  for (size_t i = 0; i < word.size() && outLen + 1 < WORD_NULL_TERM_BUF;) {
    // Skip soft hyphen (UTF-8: 0xC2 0xAD)
    if (i + 1 < word.size() && static_cast<uint8_t>(word[i]) == 0xC2 && static_cast<uint8_t>(word[i + 1]) == 0xAD) {
      i += 2;
      continue;
    }
    outBuf[outLen++] = word[i++];
  }
  if (appendHyphen && outLen + 1 < WORD_NULL_TERM_BUF) {
    outBuf[outLen++] = '-';
  }
  outBuf[outLen] = '\0';
  return outBuf;
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing. All temporary buffers are stack-allocated to avoid heap pressure during layout.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, std::string_view word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  char buf[WORD_NULL_TERM_BUF];
  const char* cstr = (hasSoftHyphen || appendHyphen) ? sanitizeWord(buf, word, appendHyphen) : nullTerminate(buf, word);
  return renderer.getTextAdvanceX(fontId, cstr, style);
}

}  // namespace

void ParsedText::pushWord(const std::string_view word, const EpdFontFamily::Style style, const bool continues) {
  const auto offset = static_cast<uint32_t>(wordArena.size());
  wordArena.insert(wordArena.end(), word.begin(), word.end());
  wordOffsets.push_back(offset);
  wordLengths.push_back(static_cast<uint16_t>(word.size()));
  wordStyles.push_back(style);
  wordContinues.push_back(continues);
}

void ParsedText::eraseFront(const size_t count) {
  if (count == 0) return;
  // Arena bytes are not compacted; the freed slots are reclaimed when the
  // ParsedText is destroyed at the end of the paragraph. Paragraph word
  // counts are bounded (the parser flushes when size > 750) so the arena
  // peak is bounded too.
  wordOffsets.erase(wordOffsets.begin(), wordOffsets.begin() + count);
  wordLengths.erase(wordLengths.begin(), wordLengths.begin() + count);
  wordStyles.erase(wordStyles.begin(), wordStyles.begin() + count);
  wordContinues.erase(wordContinues.begin(), wordContinues.begin() + count);
}

void ParsedText::addWord(const std::string_view word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;

  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  pushWord(word, combinedStyle, attachToPrevious);
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (wordOffsets.empty()) {
    return;
  }

  // Apply fixed transforms before any per-line layout work.
  applyParagraphIndent();

  const int pageWidth = viewportWidth;
  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, wordWidths, wordContinues, lineBreakIndices, processLine, renderer, fontId);
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    eraseFront(consumed);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(wordOffsets.size());

  for (size_t i = 0; i < wordOffsets.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, wordView(i), wordStyles[i]));
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec) {
  if (wordOffsets.empty()) {
    return {};
  }

  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const int firstLineIndent =
      blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = wordOffsets.size();

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      int gap = 0;
      if (j > static_cast<size_t>(i) && !continuesVec[j]) {
        gap = renderer.getSpaceAdvance(fontId, lastCodepoint(wordView(j - 1)), firstCodepoint(wordView(j)),
                                       wordStyles[j - 1]);
      } else if (j > static_cast<size_t>(i) && continuesVec[j]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        gap =
            renderer.getKerning(fontId, lastCodepoint(wordView(j - 1)), firstCodepoint(wordView(j)), wordStyles[j - 1]);
      }
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

void ParsedText::applyParagraphIndent() {
  if (extraParagraphSpacing || wordOffsets.empty()) {
    return;
  }

  if (blockStyle.textIndentDefined) {
    // CSS text-indent is explicitly set (even if 0) - don't use fallback EmSpace
    // The actual indent positioning is handled in extractLine()
    return;
  }
  if (blockStyle.alignment != CssTextAlign::Justify && blockStyle.alignment != CssTextAlign::Left) {
    return;
  }

  // No CSS text-indent defined - prepend an EmSpace to the first word as a
  // visual indent. With the arena layout we can't grow the original word in
  // place, so re-emit a modified copy at the end of the arena and rewrite the
  // first slot's offset/length to point at it.
  const uint32_t origOffset = wordOffsets.front();
  const uint16_t origLength = wordLengths.front();
  const auto newOffset = static_cast<uint32_t>(wordArena.size());
  // Reserve to keep insertion bounded to a single growth.
  wordArena.reserve(wordArena.size() + EM_SPACE_BYTES + origLength);
  wordArena.insert(wordArena.end(), EM_SPACE_UTF8, EM_SPACE_UTF8 + EM_SPACE_BYTES);
  // Append the original word bytes. The vector may have grown but data() now
  // points at the new buffer, and origOffset is still a valid index into it
  // because the prefix [0, origOffset+origLength) was preserved by the resize.
  wordArena.insert(wordArena.end(), wordArena.begin() + origOffset, wordArena.begin() + origOffset + origLength);
  wordOffsets.front() = newOffset;
  wordLengths.front() = static_cast<uint16_t>(EM_SPACE_BYTES + origLength);
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec) {
  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const int firstLineIndent =
      blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      int spacing = 0;
      if (!isFirstWord && !continuesVec[currentIndex]) {
        spacing = renderer.getSpaceAdvance(fontId, lastCodepoint(wordView(currentIndex - 1)),
                                           firstCodepoint(wordView(currentIndex)), wordStyles[currentIndex - 1]);
      } else if (!isFirstWord && continuesVec[currentIndex]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        spacing = renderer.getKerning(fontId, lastCodepoint(wordView(currentIndex - 1)),
                                      firstCodepoint(wordView(currentIndex)), wordStyles[currentIndex - 1]);
      }
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= wordOffsets.size()) {
    return false;
  }

  const auto style = wordStyles[wordIndex];

  // Hyphenator currently expects a std::string. The cost is one transient
  // heap allocation per hyphenation candidate, freed before the next word —
  // not part of the long-lived accumulation that fragments the heap, so the
  // OOM fix is unaffected.
  const std::string wordStr(wordView(wordIndex));

  // Collect candidate breakpoints (byte offsets and hyphen requirements). Language
  // pattern breaks are tried first. If they leave a large ragged gap, merge fallback
  // candidates and choose again so the visible hyphen lands closer to the line edge.
  auto breakInfos = Hyphenator::breakOffsets(wordStr, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= wordStr.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const std::string_view prefix(wordStr.data(), offset);
    const int prefixWidth = measureWordWidth(renderer, fontId, prefix, style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  const int naturalSpaceWidth = renderer.getSpaceWidth(fontId, style);
  const int excessiveRaggedGap = naturalSpaceWidth > 0 ? naturalSpaceWidth * 2 : 16;
  if (availableWidth - chosenWidth > excessiveRaggedGap) {
    auto mergedBreakInfos = Hyphenator::breakOffsets(wordStr, /*includeFallback=*/true, /*mergeFallback=*/true);
    for (const auto& info : mergedBreakInfos) {
      const size_t offset = info.byteOffset;
      if (offset == 0 || offset >= wordStr.size()) {
        continue;
      }

      const bool needsHyphen = info.requiresInsertedHyphen;
      const std::string_view prefix(wordStr.data(), offset);
      const int prefixWidth = measureWordWidth(renderer, fontId, prefix, style, needsHyphen);
      if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
        continue;
      }

      chosenWidth = prefixWidth;
      chosenOffset = offset;
      chosenNeedsHyphen = needsHyphen;
    }
  }

  // Re-emit the prefix (with optional hyphen) and the remainder at the end of
  // the arena. The original bytes become inert until the ParsedText is
  // destroyed. Resize-then-memcpy is used instead of two .insert() calls so we
  // never read from a vector iterator that a subsequent grow could invalidate.
  const size_t prefixLen = chosenOffset;
  const size_t hyphenBytes = chosenNeedsHyphen ? 1 : 0;
  const size_t remainderLen = wordStr.size() - chosenOffset;
  const size_t additional = prefixLen + hyphenBytes + remainderLen;

  const size_t arenaOldSize = wordArena.size();
  wordArena.resize(arenaOldSize + additional);
  // After resize() any pointer/iterator from before is invalid; refresh data().
  char* arena = wordArena.data();

  std::memcpy(arena + arenaOldSize, wordStr.data(), prefixLen);
  if (chosenNeedsHyphen) {
    arena[arenaOldSize + prefixLen] = '-';
  }
  std::memcpy(arena + arenaOldSize + prefixLen + hyphenBytes, wordStr.data() + chosenOffset, remainderLen);

  const auto newPrefixOffset = static_cast<uint32_t>(arenaOldSize);
  const auto newRemainderOffset = static_cast<uint32_t>(arenaOldSize + prefixLen + hyphenBytes);

  // Update the existing slot for the prefix and insert a new slot for the remainder.
  wordOffsets[wordIndex] = newPrefixOffset;
  wordLengths[wordIndex] = static_cast<uint16_t>(prefixLen + hyphenBytes);
  wordOffsets.insert(wordOffsets.begin() + wordIndex + 1, newRemainderOffset);
  wordLengths.insert(wordLengths.begin() + wordIndex + 1, static_cast<uint16_t>(remainderLen));
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const std::string_view remainderView(wordStr.data() + chosenOffset, remainderLen);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainderView, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const bool isFirstLine = breakIndex == 0;
  const int firstLineIndent =
      isFirstLine && blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a continuation
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
      totalNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(wordView(lastBreakAt + wordIdx - 1)),
                                                   firstCodepoint(wordView(lastBreakAt + wordIdx)),
                                                   wordStyles[lastBreakAt + wordIdx - 1]);
    } else if (wordIdx > 0 && continuesVec[lastBreakAt + wordIdx]) {
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      totalNaturalGaps +=
          renderer.getKerning(fontId, lastCodepoint(wordView(lastBreakAt + wordIdx - 1)),
                              firstCodepoint(wordView(lastBreakAt + wordIdx)), wordStyles[lastBreakAt + wordIdx - 1]);
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For justified text, compute per-gap extra to distribute remaining space evenly.
  // Avoid stretching short English lines into unreadable word spacing: with only
  // one or two gaps, a normal line can end up with a huge space between words.
  const int spareSpace = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  const int naturalGap = actualGapCount > 0 ? totalNaturalGaps / static_cast<int>(actualGapCount) : 0;
  const int candidateJustifyExtra =
      (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 3 && spareSpace > 0)
          ? spareSpace / static_cast<int>(actualGapCount)
          : 0;
  const int justifyExtra = (naturalGap > 0 && candidateJustifyExtra <= naturalGap * 2) ? candidateJustifyExtra : 0;

  // Calculate initial x position (first line starts at indent for left/justified text;
  // may be negative for hanging indents, e.g. margin-left:3em; text-indent:-1em).
  auto xpos = static_cast<int16_t>(firstLineIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
  }

  // Pre-calculate X positions for words
  // Continuation words attach to the previous word with no space before them
  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineXPos.push_back(xpos);

    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
    if (nextIsContinuation) {
      int advance = wordWidths[lastBreakAt + wordIdx];
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      advance +=
          renderer.getKerning(fontId, lastCodepoint(wordView(lastBreakAt + wordIdx)),
                              firstCodepoint(wordView(lastBreakAt + wordIdx + 1)), wordStyles[lastBreakAt + wordIdx]);
      xpos += advance;
    } else {
      int gap = 0;
      if (wordIdx + 1 < lineWordCount) {
        gap = renderer.getSpaceAdvance(fontId, lastCodepoint(wordView(lastBreakAt + wordIdx)),
                                       firstCodepoint(wordView(lastBreakAt + wordIdx + 1)),
                                       wordStyles[lastBreakAt + wordIdx]);
      }
      if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine) {
        gap += justifyExtra;
      }
      xpos += wordWidths[lastBreakAt + wordIdx] + gap;
    }
  }

  // Materialise the line's words as std::strings so the resulting TextBlock
  // owns its data. This is the only point in the parse pipeline where we
  // allocate per-word strings; the count is small (the line, not the whole
  // paragraph) so heap pressure is bounded.
  std::vector<std::string> lineWords;
  lineWords.reserve(lineWordCount);
  for (size_t wordIdx = 0; wordIdx < lineWordCount; ++wordIdx) {
    const std::string_view view = wordView(lastBreakAt + wordIdx);
    lineWords.emplace_back(view.data(), view.size());
  }
  std::vector<EpdFontFamily::Style> lineWordStyles(wordStyles.begin() + lastBreakAt, wordStyles.begin() + lineBreak);

  for (auto& word : lineWords) {
    if (word.find(SOFT_HYPHEN_UTF8, 0, SOFT_HYPHEN_BYTES) != std::string::npos) {
      stripSoftHyphensInPlace(word);
    }
  }

  processLine(
      std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle));
}
