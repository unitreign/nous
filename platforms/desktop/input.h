#pragma once

#include <SDL.h>

#include "nous/Input.h"
#include "runtime.h"

// Desktop input: reads SDL keyboard state and maps keys to microreader buttons.
// Requires DesktopRuntime::pump_events() to have been called this frame.
class DesktopInputSource final : public microreader::IInputSource {
 public:
  explicit DesktopInputSource(DesktopRuntime& runtime) : runtime_(runtime) {}

  microreader::ButtonState poll_buttons() override {
    runtime_.pump_events();
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    uint8_t mask = 0;
    if (keys[SDL_SCANCODE_LEFT])
      mask |= 1u << static_cast<uint8_t>(microreader::Button::Button0);
    if (keys[SDL_SCANCODE_RIGHT])
      mask |= 1u << static_cast<uint8_t>(microreader::Button::Button1);
    if (keys[SDL_SCANCODE_UP])
      mask |= 1u << static_cast<uint8_t>(microreader::Button::Button3);
    if (keys[SDL_SCANCODE_DOWN])
      mask |= 1u << static_cast<uint8_t>(microreader::Button::Button2);
    if (keys[SDL_SCANCODE_Q])
      mask |= 1u << static_cast<uint8_t>(microreader::Button::Up);
    if (keys[SDL_SCANCODE_A])
      mask |= 1u << static_cast<uint8_t>(microreader::Button::Down);
    if (keys[SDL_SCANCODE_RETURN])
      mask |= 1u << static_cast<uint8_t>(microreader::Button::Power);

    microreader::ButtonState state;
    state.current = mask;
    runtime_.consume_press_events(state.press_history, state.press_history_count, state.pressed_latch);
    return state;
  }

 private:
  DesktopRuntime& runtime_;
};
