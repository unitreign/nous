#pragma once

#include <cstdint>

namespace microreader {

// Why the reader was opened. Selecting a book from a menu is an explicit
// statement of intent, so that session counts from the first page. The boot
// auto-resume (Application::start re-opens the last book before the user has
// touched anything) is not, so it stays provisional until proven otherwise.
enum class OpenReason : uint8_t { UserOpened, AutoResumed };

// What the user just did. Only page navigation proves someone is reading —
// pressing Back to escape an unwanted auto-resume, or opening the options
// menu, must not confirm a session or invent reading time.
enum class Activity : uint8_t { PageNavigation, Other };

// Tracks how long a book has actually been *read*, as opposed to how long it
// has been open.
//
// The old accounting stamped uptime at start() and subtracted at stop(), which
// billed every menu, every idle wait and every boot auto-resume as reading —
// a book left open on a USB-powered device logged hours overnight.
//
// This owns nothing but time arithmetic: no display, no I/O, no dependency on
// Application. That keeps it unit-testable against a fake clock.
class ReadingActivityTracker {
 public:
  // Stop accruing this long after the last interaction. Generous enough for a
  // slow page at a large font size, short enough that a book left face-up on
  // the desk stops counting.
  static constexpr uint32_t kIdleTimeoutMs = 180000;  // 3 min

  void on_open(uint32_t now, OpenReason reason) {
    total_ms_ = 0;
    state_ = (reason == OpenReason::UserOpened) ? State::Confirmed : State::Provisional;
    last_tick_ms_ = now;
    last_activity_ms_ = now;
    anchored_ = true;
    pending_confirmation_ = (state_ == State::Confirmed);
  }

  // Credit the time since the last tick. Safe to call every frame; callers with
  // no input to report use this directly.
  void advance(uint32_t now) {
    if (!anchored_) {
      last_tick_ms_ = now;
      anchored_ = true;
      return;
    }
    if (state_ == State::Confirmed) {
      // Work in durations rather than comparing absolute timestamps, so the
      // ~49.7-day uint32_t wrap is handled by modular subtraction.
      //
      // Credit the overlap of [last_tick, now] with the active window that
      // ends kIdleTimeoutMs after the last interaction. A tick straddling the
      // cutoff therefore still earns its pre-cutoff portion.
      const uint32_t elapsed = now - last_tick_ms_;
      const uint32_t idle_at_tick = last_tick_ms_ - last_activity_ms_;
      const uint32_t budget = idle_at_tick >= kIdleTimeoutMs ? 0u : kIdleTimeoutMs - idle_at_tick;
      total_ms_ += (elapsed < budget) ? elapsed : budget;
    }
    last_tick_ms_ = now;
  }

  // Note an interaction. Advances first, deliberately: extending the deadline
  // before the preceding interval is evaluated would retroactively pay for an
  // idle gap the user spent away from the device.
  void on_activity(uint32_t now, Activity kind) {
    advance(now);
    if (state_ == State::Provisional && kind == Activity::PageNavigation) {
      state_ = State::Confirmed;
      pending_confirmation_ = true;
      // Counting starts here, not at open — the idle stretch before the user
      // engaged is not reading time.
    }
    if (state_ == State::Confirmed)
      last_activity_ms_ = now;
  }

  // A child screen (options, chapters, links, stats) is taking over. Drop the
  // anchor so the time spent there is not billed on the first tick after we
  // come back.
  void on_pause(uint32_t now) {
    advance(now);
    anchored_ = false;
  }

  // Back from a child screen. Re-anchors and refreshes the active window —
  // dismissing a menu is deliberate, so treat it as Activity::Other. It never
  // confirms a provisional session.
  void on_resume(uint32_t now) {
    last_tick_ms_ = now;
    anchored_ = true;
    if (state_ == State::Confirmed)
      last_activity_ms_ = now;
  }

  void on_close(uint32_t now) {
    advance(now);
    anchored_ = false;
  }

  uint64_t total_ms() const { return total_ms_; }
  bool confirmed() const { return state_ == State::Confirmed; }

  // True exactly once per session, the first time the session is confirmed.
  // Drives the "times opened" counter so idle auto-resumes stop inflating it.
  bool consume_confirmation() {
    const bool v = pending_confirmation_;
    pending_confirmation_ = false;
    return v;
  }

 private:
  enum class State : uint8_t { Provisional, Confirmed };

  uint64_t total_ms_ = 0;
  uint32_t last_tick_ms_ = 0;
  uint32_t last_activity_ms_ = 0;
  State state_ = State::Provisional;
  bool anchored_ = false;
  bool pending_confirmation_ = false;
};

}  // namespace microreader
