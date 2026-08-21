#include <cassert>

#include "lib/GfxRenderer/TextFallbackScale.h"

int main() {
  const TextFallbackScale smallCjk = makeTextFallbackScale(18, 23, 20, 20);
  assert(smallCjk.numerator == 18);
  assert(smallCjk.denominator == 20);
  assert(scaleTextFallbackExtent(20, smallCjk) == 18);
  assert(scaleTextFallbackExtent(19, smallCjk) == 18);
  assert(scaleTextFallbackMetric(-2, smallCjk) == -2);

  const TextFallbackScale uiExternal = makeTextFallbackScale(18, 23, 14, 14);
  assert(uiExternal.numerator == 1);
  assert(uiExternal.denominator == 1);

  const TextFallbackScale smallerLineBox = makeTextFallbackScale(20, 24, 24, 29);
  assert(smallerLineBox.numerator == 24);
  assert(smallerLineBox.denominator == 29);
  assert(scaleTextFallbackExtent(12, smallerLineBox) == 10);
  assert(scaleTextFallbackExtent(24, composeHalfScale(smallerLineBox, true)) == 10);

  const TextFallbackScale sampled{3, 5};
  assert(textFallbackSourceStart(0, sampled) == 0);
  assert(textFallbackSourceEnd(0, 10, sampled) == 2);
  assert(textFallbackSourceStart(1, sampled) == 1);
  assert(textFallbackSourceEnd(1, 10, sampled) == 4);

  return 0;
}
