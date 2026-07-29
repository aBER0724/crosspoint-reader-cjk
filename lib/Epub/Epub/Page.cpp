#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <new>

namespace {

template <typename Predicate>
bool renderFilteredPageElements(const std::vector<std::shared_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const int fontId, const int xOffset, const int yOffset,
                                const PageRenderCancellation* const cancellation, Predicate&& predicate) {
  for (const auto& element : elements) {
    if (cancellation && cancellation->requested()) {
      return false;
    }
    if (predicate(*element) && !element->render(renderer, fontId, xOffset, yOffset, cancellation)) {
      return false;
    }
  }
  return !cancellation || !cancellation->requested();
}

}  // namespace

bool PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                      const PageRenderCancellation* const cancellation) {
  return block->render(renderer, fontId, xPos + xOffset, yPos + yOffset, cancellation);
}

bool PageLine::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR("PGE", "Deserialization failed: null TextBlock");
    return nullptr;
  }

  auto* line = new (std::nothrow) PageLine(std::move(tb), xPos, yPos);
  if (!line) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  return std::unique_ptr<PageLine>(line);
}

bool PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                       const PageRenderCancellation* const cancellation) {
  // Images don't use fontId or text rendering
  if (cancellation && cancellation->requested()) {
    return false;
  }
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
  return !cancellation || !cancellation->requested();
}

void PageImage::renderPlaceholder(GfxRenderer& renderer, const int xOffset, const int yOffset) const {
  imageBlock->renderPlaceholder(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto ib = ImageBlock::deserialize(file);
  return std::unique_ptr<PageImage>(new PageImage(std::move(ib), xPos, yPos));
}

bool PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                                const PageRenderCancellation* const cancellation) {
  (void)fontId;
  if (cancellation && cancellation->requested()) {
    return false;
  }
  if (width == 0 || thickness == 0) {
    return true;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness, true);
  return !cancellation || !cancellation->requested();
}

bool PageHorizontalRule::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  serialization::writePod(file, thickness);
  return true;
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, width);
  serialization::readPod(file, thickness);

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto* rule = new (std::nothrow) PageHorizontalRule(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return std::unique_ptr<PageHorizontalRule>(rule);
}

bool Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                  const PageRenderCancellation* const cancellation) const {
  return renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, cancellation,
                                    [](const PageElement&) { return true; });
}

bool Page::renderImages(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                        const PageRenderCancellation* const cancellation) const {
  return renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, cancellation,
                                    [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

bool Page::renderWithImagePlaceholders(GfxRenderer& renderer, const int fontId, const int xOffset,
                                       const int yOffset, const PageRenderCancellation* const cancellation) const {
  for (const auto& element : elements) {
    if (cancellation && cancellation->requested()) {
      return false;
    }
    if (element->getTag() == TAG_PageImage) {
      static_cast<const PageImage&>(*element).renderPlaceholder(renderer, xOffset, yOffset);
    } else if (!element->render(renderer, fontId, xOffset, yOffset, cancellation)) {
      return false;
    }
  }
  return !cancellation || !cancellation->requested();
}

bool Page::serialize(HalFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  serialization::writePod(file, fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint16_t count;
  serialization::readPod(file, count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  serialization::readPod(file, fnCount);
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  return page;
}
