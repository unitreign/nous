#pragma once

#include <cstdint>

namespace microreader {

enum class Button : uint8_t { Button0 = 0, Button1 = 1, Button2 = 2, Button3 = 3, Up = 4, Down = 5, Power = 6 };

struct ButtonState {
  static constexpr uint8_t kButtonCount = 7;
  static constexpr uint8_t kMaxPressHistory = 16;
  static constexpr uint32_t kRepeatDelayMs = 350;
  static constexpr uint32_t kRepeatIntervalMs = 175;

  uint8_t current = 0;        // Instantaneous state at last sample
  uint8_t pressed_latch = 0;  // Accumulated rising edges since last poll (backward compat)

  // Ordered history of button presses since the last poll (oldest first).
  // Stores button indices (cast to Button). Capped at kMaxPressHistory entries;
  // excess events are silently dropped.
  uint8_t press_history[kMaxPressHistory] = {};
  uint8_t press_history_count = 0;

  bool is_down(Button button) const {
    const uint8_t mask = static_cast<uint8_t>(1u << static_cast<uint8_t>(button));
    return (current & mask) != 0;
  }

  bool is_pressed(Button button) const {
    const uint8_t mask = static_cast<uint8_t>(1u << static_cast<uint8_t>(button));
    return (pressed_latch & mask) != 0;
  }

  // Consume the next press event in arrival order. Returns false when exhausted.
  // The read cursor is mutable so this works on const ButtonState references.
  bool next_press(Button& out) const {
    if (press_history_pos_ >= press_history_count)
      return false;
    out = static_cast<Button>(press_history[press_history_pos_++]);
    return true;
  }

 private:
  mutable uint8_t press_history_pos_ = 0;
};

class IInputSource {
 public:
  virtual ~IInputSource() = default;
  virtual ButtonState poll_buttons() = 0;
};

}  // namespace microreader
