#include "Loop.h"

#include <algorithm>
#include <chrono>

namespace microreader {

// Measured time since the previous update, so e-ink refreshes (often several
// hundred ms) count fully. Capped so a long blocking job like a conversion
// doesn't count as idle time and trigger auto-sleep right after it finishes.
static uint32_t elapsed_ms_(IRuntime& runtime) {
  using clock = std::chrono::steady_clock;
  static clock::time_point last{};
  static bool have_last = false;
  const clock::time_point now = clock::now();
  uint32_t dt = runtime.frame_time_ms();
  if (have_last) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    dt = static_cast<uint32_t>(std::clamp<long long>(ms, 0, 1000));
  }
  last = now;
  have_last = true;
  return dt;
}

void run_loop_iteration(Application& app, DrawBuffer& buf, IInputSource& input, IRuntime& runtime) {
  const ButtonState buttons = input.poll_buttons();
  if (runtime.step_mode() && !runtime.consume_step()) {
    runtime.wait_next_frame();
    return;
  }
  app.update(buttons, elapsed_ms_(runtime), buf, runtime);
  runtime.wait_next_frame();
}

void run_loop(Application& app, DrawBuffer& buf, IInputSource& input, IRuntime& runtime) {
  while (runtime.should_continue() && app.running())
    run_loop_iteration(app, buf, input, runtime);
}

}  // namespace microreader
