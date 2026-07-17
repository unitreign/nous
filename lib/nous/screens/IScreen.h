#pragma once

#include "../Input.h"
#include "../Runtime.h"
#include "../display/DrawBuffer.h"

namespace microreader {

class Application;  // back-reference — include Application.h in screen .cpp files

// Interface for application screens that Application can switch between.
class IScreen {
 public:
  virtual ~IScreen() = default;

  void set_app(Application* app) {
    app_ = app;
  }

  Application* app() const {
    return app_;
  }

  virtual const char* name() const = 0;

  // Called once when this screen becomes active.
  // Draws initial content into buf; caller is responsible for buf.refresh().
  virtual void start(DrawBuffer& buf, IRuntime& runtime) = 0;

  // Called once when leaving this screen permanently (being removed from the stack).
  virtual void stop() = 0;

  // Called when a child screen is pushed on top (screen stays on the stack but loses focus).
  // Default: same as stop(). Override to keep resources alive across child screens.
  virtual void pause() {
    stop();
  }

  // Called when returning to this screen after a child was popped.
  // Default: same as start(). Override to resume cheaply without full reinitialisation.
  virtual void resume(DrawBuffer& buf, IRuntime& runtime) {
    start(buf, runtime);
  }

  // Per-frame update. Call app_->pop_screen() to exit.
  // Call buf.refresh() internally when the display needs updating.
  virtual void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) = 0;

 protected:
  Application* app_ = nullptr;
};

}  // namespace microreader
