#include "MappedInputManager.h"

#include <algorithm>
#include <numeric>

#include "CrossPointSettings.h"
#include "input/BluetoothPageTurnState.h"

// Button mapping is split into three orthogonal concerns:
//
//   1. **Hardware spec**  — which GPIO index sits at which physical position
//      on the X4 device. Source of truth: the `kFront*` / `kSide*` constants
//      below. This is the only place that touches HalGPIO::BTN_* values.
//
//   2. **User remap**     — the user can re-assign the four front-bezel
//      "roles" (Back / Confirm / Left / Right) to arbitrary hardware indices,
//      and choose a side-strip prev/next polarity (PREV_NEXT / NEXT_PREV).
//      Lives in `CrossPointSettings::frontButton*` and `sideButtonLayout`.
//
//   3. **Orientation**    — when the device is rotated/flipped, the same
//      physical button now sits at a different *visual* position. Each
//      logical button has its own rule for how to react to orientation:
//
//          | Logical button         | Inverted | LandscapeCW | LandscapeCCW |
//          |------------------------|----------|-------------|--------------|
//          | Back / Confirm         | mirror   |    —        |     —        |
//          | Left / Right           | mirror   |    —        |  swap roles  |
//          | Up / Down              | (table)  |   (table)   |   (table)    |
//          | PageBack / PageForward | (table)  |   (table)   |   (table)    |
//          | Power                  |    —     |    —        |     —        |
//
//      "mirror" = front-bezel hw index 0..3 is reflected (3 - i).
//      "swap"   = the BTN_UP / BTN_DOWN hardware names are exchanged.
//      "swap roles" = the role looked up in SETTINGS is the opposite arrow
//      (Left looks up Right, Right looks up Left), without an index mirror.
//      "(table)" = PageBack/PageForward use kPageBackHwByOrientation, an
//      empirically-derived per-orientation table (see comment by the table
//      definition). The user's SIDE_BUTTON_LAYOUT setting then applies a
//      uniform global flip to every entry.
//
// Each rule is documented next to the corresponding case below so the
// behaviour can be audited without reading the whole file.

namespace {
using ButtonIndex = uint8_t;
using Orientation = MappedInputManager::Orientation;

// === Hardware spec (X4) ===
//
// Front bezel: four buttons along the bottom edge of the device when held
// in portrait, ordered left-to-right with hardware indices 0..3. The
// HalGPIO names happen to follow this physical order.
//
// Side strip: two buttons on the right edge of the device when held in
// portrait, ordered top-to-bottom. ADC channel naming != physical position:
//   BTN_DOWN(5) is the physical UPPER button.
//   BTN_UP(4)   is the physical LOWER button.
// (We refer to them by HalGPIO name in the side-strip code below; the
// kPageBackHwByOrientation table encodes the per-orientation mapping
// directly so we don't need named "upper"/"lower" aliases.)

// === Side polarity (user-configurable) ===
//
// `kPageBackHwByOrientation` is the empirically-validated table of which
// physical side button on the X4 the user perceives as "previous page" in
// each orientation. Each rotation maps the right-edge side strip onto a
// different physical edge of the device:
//
//   Portrait                   — strip on right edge,  prev = BTN_UP   (lower of right edge)
//   PortraitInverted           — strip on left edge,   prev = BTN_DOWN (now lower of left edge)
//   LandscapeClockwise         — strip on bottom edge, prev = BTN_DOWN (now bottom-right)
//   LandscapeCounterClockwise  — strip on top edge,    prev = BTN_UP   (now top-right)
//
// This is asymmetric (Portrait + LandscapeCCW pick BTN_UP; the other two
// pick BTN_DOWN) because the X4's right-edge strip lands at the user's
// natural-thumb side in those two orientations, and on the opposite side
// in the other two. Validated against on-device button presses.
//
// Users who prefer the opposite mapping in every orientation can toggle
// SIDE_BUTTON_LAYOUT in settings; that flips every entry of this table
// uniformly via `flipSideButton`.
constexpr ButtonIndex kPageBackHwByOrientation[] = {
    HalGPIO::BTN_UP,    // Portrait
    HalGPIO::BTN_DOWN,  // PortraitInverted
    HalGPIO::BTN_DOWN,  // LandscapeClockwise
    HalGPIO::BTN_UP,    // LandscapeCounterClockwise
};
static_assert(sizeof(kPageBackHwByOrientation) / sizeof(ButtonIndex) == 4,
              "kPageBackHwByOrientation must cover every Orientation enum value");

constexpr ButtonIndex flipSideButton(ButtonIndex hw) {
  return hw == HalGPIO::BTN_UP ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
}

// === Helpers ===
//
// Mirror a front-bezel hardware index across the centre (0<->3, 1<->2).
// Used when the device is held inverted: the user's role-assigned index
// is in portrait coordinates, so to address the same visual position we
// have to reflect it.
constexpr ButtonIndex mirrorFront(ButtonIndex idx) { return 3 - idx; }

// `isInverted`           — device flipped 180° relative to portrait.
//                          Triggers index/polarity mirroring for buttons
//                          whose visual position depends on orientation.
// `isLandscapeRotated`   — device rotated 90° (either direction). Side-
//                          strip paging buttons swap polarity in either
//                          rotation; front Left/Right swap only on CCW
//                          (the bezel ends up on the right edge with
//                          reversed top-to-bottom order).
constexpr bool isInverted(Orientation o) { return o == Orientation::PortraitInverted; }
constexpr bool isLandscapeRotated(Orientation o) {
  return o == Orientation::LandscapeClockwise || o == Orientation::LandscapeCounterClockwise;
}
constexpr bool isLandscapeCcw(Orientation o) { return o == Orientation::LandscapeCounterClockwise; }

// Resolve a user-assigned front-bezel role to its active hardware index.
// Inverted reflects the index; landscape rotations don't (the bezel stays
// addressable by the same hardware index, only its visual position moves).
constexpr ButtonIndex frontHwForRole(ButtonIndex roleIndex, Orientation o) {
  return isInverted(o) ? mirrorFront(roleIndex) : roleIndex;
}

}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const Orientation o = effectiveOrientation;
  const auto layout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);

  // Resolve the side-strip "previous" hardware index for the active
  // orientation, applying the user's polarity flip if SIDE_BUTTON_LAYOUT
  // is set to NEXT_PREV.
  const ButtonIndex defaultPrev = kPageBackHwByOrientation[static_cast<size_t>(o)];
  const ButtonIndex sidePrevHw =
      layout == CrossPointSettings::SIDE_BUTTON_LAYOUT::NEXT_PREV ? flipSideButton(defaultPrev) : defaultPrev;
  const ButtonIndex sideNextHw = flipSideButton(sidePrevHw);

  switch (button) {
    // === Front-bezel action buttons =====================================
    // The user assigns each role (Back / Confirm) to a specific hardware
    // index via SETTINGS. Inverted mirrors the index; landscape rotations
    // do not — the user finds these buttons by their on-screen label,
    // which the renderer redraws at the correct edge for each orientation.
    case Button::Back:
      return (gpio.*fn)(frontHwForRole(SETTINGS.frontButtonBack, o));
    case Button::Confirm:
      return (gpio.*fn)(frontHwForRole(SETTINGS.frontButtonConfirm, o));

    // === Front-bezel directional buttons ================================
    // In LandscapeCCW the bezel rotates onto the device's right edge with
    // reversed top-to-bottom order, so the user's "Left" arrow addresses
    // the role that was originally on the right (and vice versa). The
    // index itself is NOT mirrored: in CCW rotation the role-assigned hw
    // index still hits the right physical button. In Inverted, both the
    // role-swap is unnecessary AND the index needs reflection.
    case Button::Left:
      if (isInverted(o)) return (gpio.*fn)(mirrorFront(SETTINGS.frontButtonLeft));
      if (isLandscapeCcw(o)) return (gpio.*fn)(SETTINGS.frontButtonRight);
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      if (isInverted(o)) return (gpio.*fn)(mirrorFront(SETTINGS.frontButtonRight));
      if (isLandscapeCcw(o)) return (gpio.*fn)(SETTINGS.frontButtonLeft);
      return (gpio.*fn)(SETTINGS.frontButtonRight);

    // === Side-strip vertical (Up / Down) ================================
    // Used by ButtonNavigator as side-key alternatives to Left/Right for
    // chapter / menu navigation. Up = "scroll up the list" = same physical
    // button as PageBack; Down = same as PageForward. Both follow the
    // per-orientation kPageBackHwByOrientation table so the side button
    // physically nearest the user's natural-thumb position always performs
    // the "previous" direction in every orientation, mirroring reader
    // paging. Required for the chapter list to feel consistent in
    // landscape rotations (CW was the orientation that exposed this; the
    // previous "swap on Inverted only" rule misfired there).
    case Button::Up:
      return (gpio.*fn)(sidePrevHw);
    case Button::Down:
      return (gpio.*fn)(sideNextHw);

    // === Reader paging (PageBack / PageForward) =========================
    // Driven by the `kPageBackHwByOrientation` table at the top of this
    // file: each orientation has its own empirically-validated answer for
    // which physical side button is "previous" on this user's X4 unit.
    // The user's SIDE_BUTTON_LAYOUT setting (PREV_NEXT / NEXT_PREV) then
    // applies a uniform global flip, in case they prefer the opposite
    // mapping in every orientation.
    case Button::PageBack:
      return (gpio.*fn)(sidePrevHw);
    case Button::PageForward:
      return (gpio.*fn)(sideNextHw);

    // === Power ==========================================================
    case Button::Power:
      return (gpio.*fn)(HalGPIO::BTN_POWER);
  }

  return false;  // unreachable; switch covers all enum values.
}

bool MappedInputManager::checkButton(const Button button, const GpioFn gpioFn, const BtFn btPageBackFn,
                                     const BtFn btPageForwardFn) const {
  const bool physical = mapButton(button, gpioFn);
  if (!bluetoothPageTurnState) {
    return physical;
  }
  switch (button) {
    case Button::PageBack:
      return physical || (bluetoothPageTurnState->*btPageBackFn)();
    case Button::PageForward:
      return physical || (bluetoothPageTurnState->*btPageForwardFn)();
    default:
      return physical;
  }
}

bool MappedInputManager::wasPressed(const Button button) const {
  const bool physical = checkButton(button, &HalGPIO::wasPressed, &BluetoothPageTurnState::wasPageBackPressed,
                                    &BluetoothPageTurnState::wasPageForwardPressed);
#ifdef ENABLE_SERIAL_INPUT_TEST
  return physical || testButtons[static_cast<size_t>(button)].pressed;
#else
  return physical;
#endif
}

bool MappedInputManager::wasReleased(const Button button) const {
  const bool physical = checkButton(button, &HalGPIO::wasReleased, &BluetoothPageTurnState::wasPageBackReleased,
                                    &BluetoothPageTurnState::wasPageForwardReleased);
#ifdef ENABLE_SERIAL_INPUT_TEST
  return physical || testButtons[static_cast<size_t>(button)].released;
#else
  return physical;
#endif
}

bool MappedInputManager::isPressed(const Button button) const {
  const bool physical = checkButton(button, &HalGPIO::isPressed, &BluetoothPageTurnState::isPageBackPressed,
                                    &BluetoothPageTurnState::isPageForwardPressed);
#ifdef ENABLE_SERIAL_INPUT_TEST
  return physical || testButtons[static_cast<size_t>(button)].held;
#else
  return physical;
#endif
}

bool MappedInputManager::wasAnyPressed() const {
  if (gpio.wasAnyPressed() || (bluetoothPageTurnState && bluetoothPageTurnState->wasAnyPressed())) {
    return true;
  }
#ifdef ENABLE_SERIAL_INPUT_TEST
  return std::any_of(testButtons.begin(), testButtons.end(),
                     [](const TestButtonState& state) { return state.pressed; });
#else
  return false;
#endif
}

bool MappedInputManager::wasAnyReleased() const {
  if (gpio.wasAnyReleased() || (bluetoothPageTurnState && bluetoothPageTurnState->wasAnyReleased())) {
    return true;
  }
#ifdef ENABLE_SERIAL_INPUT_TEST
  return std::any_of(testButtons.begin(), testButtons.end(),
                     [](const TestButtonState& state) { return state.released; });
#else
  return false;
#endif
}

unsigned long MappedInputManager::getHeldTime() const {
  unsigned long heldTime = gpio.getHeldTime();
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
  // Build the label order based on the configured hardware mapping.
  //
  // The returned Labels[] is indexed by the four front-bezel hardware slots
  // (BACK, CONFIRM, LEFT, RIGHT). Theme code (drawButtonHints) places each
  // label at the *physical* position of its hardware button — including the
  // vertical strip layout used in landscape orientations — so this routine
  // only has to compensate for a single asymmetric remap that mapButton
  // applies to LandscapeCCW: in CCW, Button::Left is wired to the user's
  // configured "Right" front-button hardware index (and vice versa) because
  // the bezel rotates onto the right edge with reversed top-to-bottom order.
  //
  // To keep label ↔ action consistent in CCW, the "previous"/"next" labels
  // also need to swap when looked up against the LEFT/RIGHT hardware slots.
  // Other orientations don't need this swap (mapButton only does the role
  // remap in CCW; Inverted's mirror is on the hardware index, not the role).
  const bool swapPrevNext = effectiveOrientation == Orientation::LandscapeCounterClockwise;
  const char* prev = swapPrevNext ? next : previous;
  const char* nxt = swapPrevNext ? previous : next;

  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return prev;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return nxt;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
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
