#include "MappedInputManager.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdlib>
#include <numeric>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "input/BluetoothPageTurnState.h"
#include "input/FrontButtonOrientation.h"

MappedInputManager::Orientation MappedInputManager::currentOrientation() const {
  if (!renderer) return effectiveOrientation;
  switch (renderer->getOrientation()) {
    case GfxRenderer::PortraitInverted:
      return Orientation::PortraitInverted;
    case GfxRenderer::LandscapeClockwise:
      return Orientation::LandscapeClockwise;
    case GfxRenderer::LandscapeCounterClockwise:
      return Orientation::LandscapeCounterClockwise;
    case GfxRenderer::Portrait:
    default:
      return Orientation::Portrait;
  }
}

namespace {
FrontButtonOrientation::Orientation toFrontButtonOrientation(const MappedInputManager::Orientation orientation) {
  switch (orientation) {
    case MappedInputManager::Orientation::PortraitInverted:
      return FrontButtonOrientation::Orientation::PortraitInverted;
    case MappedInputManager::Orientation::LandscapeClockwise:
      return FrontButtonOrientation::Orientation::LandscapeClockwise;
    case MappedInputManager::Orientation::LandscapeCounterClockwise:
      return FrontButtonOrientation::Orientation::LandscapeCounterClockwise;
    case MappedInputManager::Orientation::Portrait:
    default:
      return FrontButtonOrientation::Orientation::Portrait;
  }
}
}  // namespace

bool MappedInputManager::isNavDirectionSwapped() const {
  return FrontButtonOrientation::navigationSwapped(SETTINGS.frontButtonFollowOrientation,
                                                   toFrontButtonOrientation(currentOrientation()));
}

uint8_t MappedInputManager::mapFrontHardwareIndex(const uint8_t hardwareIndex) const {
  return FrontButtonOrientation::inputHardwareIndex(SETTINGS.frontButtonFollowOrientation,
                                                    toFrontButtonOrientation(currentOrientation()), hardwareIndex);
}

uint8_t MappedInputManager::hintFrontHardwareIndex(const uint8_t screenSlotIndex) const {
  return FrontButtonOrientation::hintHardwareIndex(SETTINGS.frontButtonFollowOrientation,
                                                   toFrontButtonOrientation(currentOrientation()), screenSlotIndex);
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      return (gpio.*fn)(mapFrontHardwareIndex(SETTINGS.frontButtonBack));
    case Button::Confirm:
      return (gpio.*fn)(mapFrontHardwareIndex(SETTINGS.frontButtonConfirm));
    case Button::Left:
      return (gpio.*fn)(mapFrontHardwareIndex(SETTINGS.frontButtonLeft));
    case Button::Right:
      return (gpio.*fn)(mapFrontHardwareIndex(SETTINGS.frontButtonRight));
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_UP);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(HalGPIO::BTN_UP);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::NavNext:
      // Logical "next item" navigation: side Down + front Right, with the control axis flipped in
      // INVERTED / LANDSCAPE_CCW (frontButtonFollowOrientation) so it matches the rotated hint labels.
      return isNavDirectionSwapped() ? (mapButton(Button::Up, fn) || mapButton(Button::Left, fn))
                                     : (mapButton(Button::Down, fn) || mapButton(Button::Right, fn));
    case Button::NavPrevious:
      // Logical "previous item" navigation: side Up + front Left, axis-flipped in the same orientations.
      return isNavDirectionSwapped() ? (mapButton(Button::Down, fn) || mapButton(Button::Right, fn))
                                     : (mapButton(Button::Up, fn) || mapButton(Button::Left, fn));
  }

  return false;
}

bool MappedInputManager::checkBluetooth(const Button button, const BtFn pageBackFn, const BtFn pageForwardFn) const {
  if (!bluetoothPageTurnState) return false;
  switch (button) {
    case Button::PageBack:
      return (bluetoothPageTurnState->*pageBackFn)();
    case Button::PageForward:
      return (bluetoothPageTurnState->*pageForwardFn)();
    default:
      return false;
  }
}

namespace {
constexpr float LEFT_EDGE_BACK_GESTURE_FRAC_X = 0.25f;
constexpr float BOTTOM_EDGE_BACK_GESTURE_FRAC_Y = 0.14f;
constexpr float TOP_EDGE_MENU_GESTURE_FRAC_Y = 0.14f;
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
}  // namespace

bool MappedInputManager::hasTouch() const { return renderer && gpio.hasTouch(); }

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  if (!renderer) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer->tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  if (!renderer) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer->tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  if (!renderer) return false;
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer->tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

bool MappedInputManager::listItemFromPoint(const int x, const int y, int& index, const int itemCount,
                                           const int selectedIndex, const int listTop, const int listHeight,
                                           const bool hasSubtitle) const {
  (void)x;
  if (itemCount <= 0) return false;
  if (y < listTop || y >= listTop + listHeight) return false;

  const auto& theme = UITheme::getInstance().getTheme();
  const int rowStep = theme.getListRowStep(hasSubtitle);
  if (rowStep <= 0) return false;

  const int pageItems = theme.getListPageItems(listHeight, hasSubtitle);
  if (pageItems <= 0) return false;
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int tapped = pageStart + row;
  if (row < 0 || row >= pageItems || tapped >= itemCount) return false;
  index = tapped;
  return true;
}

bool MappedInputManager::wasListItemTapped(int& index, const int itemCount, const int selectedIndex, const int listTop,
                                           const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::wasListItemTouchedDown(int& index, const int itemCount, const int selectedIndex,
                                                const int listTop, const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTouchDown(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  if (!renderer) return false;
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer->tapToLogical(nxs, nys, sx, sy);
  renderer->tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (std::abs(dx) >= std::abs(dy)) {
    return dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  }
  return dy < 0 ? SwipeDir::Up : SwipeDir::Down;
}

bool MappedInputManager::wasBackGesture() const {
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. percent selection, image viewer).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = sx <= renderer->getScreenWidth() * LEFT_EDGE_BACK_GESTURE_FRAC_X && ex > sx &&
                   std::abs(ex - sx) > std::abs(ey - sy);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasMenuGesture() const {
  // Downward swipe starting at the top edge (mirror of the bottom-edge home gesture).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer->getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const bool hit = sy <= topEdgeBottom && ey > sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasHomeGesture() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (decodeSwipe(sx, sy, ex, ey)) {
    const int bottomEdgeTop =
        renderer->getScreenHeight() - static_cast<int>(renderer->getScreenHeight() * BOTTOM_EDGE_BACK_GESTURE_FRAC_Y);
    if (sy >= bottomEdgeTop && ey < sy && std::abs(ey - sy) > std::abs(ex - sx)) {
      rememberTouchHeldTime();
      return true;
    }
  }
  return false;
}

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  const bool physical =
      mapButton(button, &HalGPIO::wasPressed) || checkBluetooth(button, &BluetoothPageTurnState::wasPageBackPressed,
                                                                &BluetoothPageTurnState::wasPageForwardPressed);
#ifdef ENABLE_SERIAL_INPUT_TEST
  return physical || testButtons[static_cast<size_t>(button)].pressed;
#else
  return physical;
#endif
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  const bool physical =
      mapButton(button, &HalGPIO::wasReleased) || checkBluetooth(button, &BluetoothPageTurnState::wasPageBackReleased,
                                                                 &BluetoothPageTurnState::wasPageForwardReleased);
#ifdef ENABLE_SERIAL_INPUT_TEST
  return physical || testButtons[static_cast<size_t>(button)].released;
#else
  return physical;
#endif
}

bool MappedInputManager::isPressed(const Button button) const {
  const bool physical =
      mapButton(button, &HalGPIO::isPressed) ||
      checkBluetooth(button, &BluetoothPageTurnState::isPageBackPressed, &BluetoothPageTurnState::isPageForwardPressed);
#ifdef ENABLE_SERIAL_INPUT_TEST
  return physical || testButtons[static_cast<size_t>(button)].held;
#else
  return physical;
#endif
}

bool MappedInputManager::wasAnyPressed() const {
  if (gpio.wasAnyPressed() || (bluetoothPageTurnState && bluetoothPageTurnState->wasAnyPressed())) return true;
#ifdef ENABLE_SERIAL_INPUT_TEST
  return std::any_of(testButtons.begin(), testButtons.end(),
                     [](const TestButtonState& state) { return state.pressed; });
#else
  return false;
#endif
}

bool MappedInputManager::wasAnyReleased() const {
  if (gpio.wasAnyReleased() || (bluetoothPageTurnState && bluetoothPageTurnState->wasAnyReleased())) return true;
#ifdef ENABLE_SERIAL_INPUT_TEST
  return std::any_of(testButtons.begin(), testButtons.end(),
                     [](const TestButtonState& state) { return state.released; });
#else
  return false;
#endif
}

unsigned long MappedInputManager::getHeldTime() const {
  unsigned long heldTime = gpio.getHeldTime();
  if (!gpio.wasAnyPressed() && !gpio.wasAnyReleased() && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    heldTime = std::max(heldTime, touchHeldOverrideMs);
  } else {
    touchHeldOverrideValid = false;
  }
#ifdef ENABLE_SERIAL_INPUT_TEST
  const unsigned long testHeldTime = std::accumulate(
      testButtons.begin(), testButtons.end(), 0UL, [](const unsigned long maximum, const TestButtonState& state) {
        return state.held ? std::max(maximum, millis() - state.pressedAt) : maximum;
      });
  heldTime = std::max(heldTime, testHeldTime);
#endif
  return heldTime;
}

#ifdef ENABLE_SERIAL_INPUT_TEST
void MappedInputManager::beginTestInputFrame() {
  std::for_each(testButtons.begin(), testButtons.end(), [](TestButtonState& state) {
    state.pressed = false;
    state.released = false;
  });
}

void MappedInputManager::setTestButtonPressed(const Button button, const bool pressed) {
  TestButtonState& state = testButtons[static_cast<size_t>(button)];
  if (pressed) {
    if (!state.held) {
      state.held = true;
      state.pressed = true;
      state.pressedAt = millis();
    }
    return;
  }
  if (state.held) {
    state.held = false;
    state.released = true;
    state.pressedAt = 0;
  }
}

void MappedInputManager::tapTestButton(const Button button) {
  TestButtonState& state = testButtons[static_cast<size_t>(button)];
  state.held = false;
  state.pressed = true;
  state.released = true;
  state.pressedAt = 0;
}

void MappedInputManager::clearTestButtons() { testButtons.fill({}); }
#endif

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  const bool swapLabels = isNavDirectionSwapped();
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  // Resolve each visible screen slot to the hardware role that remains beneath it after rotation. When front
  // buttons follow inverted portrait, input roles are mirrored and labels keep their configured slot order. When
  // following is disabled, input stays raw and the labels mirror instead so they never advertise the wrong key.
  const auto labelForScreenSlot = [&](const uint8_t screenSlot) -> const char* {
    const uint8_t hardwareIndex = hintFrontHardwareIndex(screenSlot);
    if (hardwareIndex == SETTINGS.frontButtonBack) return back;
    if (hardwareIndex == SETTINGS.frontButtonConfirm) return confirm;
    if (hardwareIndex == SETTINGS.frontButtonLeft) return leftLabel;
    if (hardwareIndex == SETTINGS.frontButtonRight) return rightLabel;
    return "";
  };

  return {labelForScreenSlot(HalGPIO::BTN_BACK), labelForScreenSlot(HalGPIO::BTN_CONFIRM),
          labelForScreenSlot(HalGPIO::BTN_LEFT), labelForScreenSlot(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
