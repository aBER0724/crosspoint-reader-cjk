#pragma once

#include <HalGPIO.h>

#ifdef ENABLE_SERIAL_INPUT_TEST
#include <array>
#include <cstddef>
#endif

class BluetoothPageTurnState;
class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward, NavNext, NavPrevious };
  enum class SwipeDir { None, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  // Keep the fork Bluetooth-page-turn constructor shape; renderer is attached after its
  // global is constructed so upstream touch input can use the same manager.
  explicit MappedInputManager(HalGPIO& gpio, const BluetoothPageTurnState* bluetoothPageTurnState = nullptr)
      : gpio(gpio), bluetoothPageTurnState(bluetoothPageTurnState) {}

  void setRenderer(const GfxRenderer& value) { renderer = &value; }

  void update() const { gpio.update(); }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  bool wasTapInRect(int x, int y, int width, int height) const;
  bool wasListItemTapped(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  bool wasListItemTouchedDown(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                              bool hasSubtitle) const;

  // Combined touch interaction for a band of equal rows with caller-supplied
  // geometry — the shared hit-test for lists the theme helpers above do not
  // cover (custom row heights, option prompts, menus). Down = a held
  // tap-candidate is on a row (update the selection highlight); Tap = a tap
  // released on one (activate). rowHeight limits the hit to the top rowHeight
  // px of each step (0 = the full step, no gap band).
  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe() const;
  bool wasHomeGesture() const;
  bool wasMenuGesture() const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  const GfxRenderer& getRenderer() const { return *renderer; }
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True when the control axis is flipped relative to the physical buttons: the user opted into
  // orientation-following front buttons AND the screen is *currently rendered* rotated (INVERTED /
  // LANDSCAPE_CCW). Keyed on the live renderer orientation rather than the persisted reader setting,
  // so portrait UI (home, settings) never swaps while the reader and its menus do.
  [[nodiscard]] bool isNavDirectionSwapped() const;

  // OrientationHelper sets the live activity orientation as it transitions between
  // portrait UI and the rotated reader. Keep this fork contract alongside the renderer.
  enum class Orientation { Portrait, PortraitInverted, LandscapeClockwise, LandscapeCounterClockwise };
  void setEffectiveOrientation(Orientation orientation) { effectiveOrientation = orientation; }

#ifdef ENABLE_SERIAL_INPUT_TEST
  void beginTestInputFrame();
  void setTestButtonPressed(Button button, bool pressed);
  void tapTestButton(Button button);
  void clearTestButtons();
#endif

 private:
  using BtFn = bool (BluetoothPageTurnState::*)() const;

  HalGPIO& gpio;
  const BluetoothPageTurnState* bluetoothPageTurnState = nullptr;
  // Logical-to-physical button mapping depends on what the user is actually looking at: when the
  // screen is rendered rotated, the directional buttons must flip to match. The renderer is the only
  // authority on the *live* orientation (the reader rotates it and restores portrait on exit), so we
  // read it here instead of CrossPointSettings.orientation, which is just the persisted reader
  // preference and stays "rotated" even while portrait UI like home/settings is on screen.
  const GfxRenderer* renderer = nullptr;
  Orientation effectiveOrientation = Orientation::Portrait;


  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  bool checkBluetooth(Button button, BtFn pageBackFn, BtFn pageForwardFn) const;

#ifdef ENABLE_SERIAL_INPUT_TEST
  struct TestButtonState {
    bool pressed = false;
    bool released = false;
    bool held = false;
    unsigned long pressedAt = 0;
  };
  static constexpr size_t TEST_BUTTON_COUNT = static_cast<size_t>(Button::NavPrevious) + 1;
  std::array<TestButtonState, TEST_BUTTON_COUNT> testButtons;
#endif
  bool wasBackGesture() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
  bool listItemFromPoint(int x, int y, int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  void rememberTouchHeldTime() const;

  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
};
