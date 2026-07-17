#include "Loop.h"

namespace microreader {

void run_loop_iteration(Application& app, DrawBuffer& buf, IInputSource& input, IRuntime& runtime) {
  const ButtonState buttons = input.poll_buttons();
  if (runtime.step_mode() && !runtime.consume_step()) {
    runtime.wait_next_frame();
    return;
  }
  app.update(buttons, runtime.frame_time_ms(), buf, runtime);
  runtime.wait_next_frame();
}

void run_loop(Application& app, DrawBuffer& buf, IInputSource& input, IRuntime& runtime) {
  while (runtime.should_continue() && app.running())
    run_loop_iteration(app, buf, input, runtime);
}

}  // namespace microreader
