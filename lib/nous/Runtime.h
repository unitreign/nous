#pragma once

#include <cstdint>
#include <optional>

#include "Input.h"

namespace microreader {

class IRuntime {
 public:
  virtual ~IRuntime() = default;

  virtual bool should_continue() const = 0;
  virtual uint32_t frame_time_ms() const = 0;
  virtual void wait_next_frame() = 0;

  // Read battery level (0-100 percentage).
  // Returns empty optional if the platform does not have a battery.
  virtual std::optional<uint8_t> battery_percentage() const = 0;

  // Optional step-mode support for debugging.
  // step_mode() returns true when the loop should pause between ticks.
  // consume_step() returns true (exactly once per press) when the user
  // has requested one tick to advance.  Default: always run freely.
  virtual bool step_mode() const {
    return false;
  }
  virtual bool consume_step() {
    return false;
  }

  // Yield to the OS/RTOS scheduler.  Call inside tight loops to avoid
  // starving background tasks (e.g. the ESP32 watchdog).
  virtual void yield() {}
};

}  // namespace microreader
