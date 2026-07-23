// ReadingActivityTrackerTest.cpp — the reading-time accounting rules.
//
// Background: reading times were wildly inflated because the old accounting
// stamped uptime at start() and subtracted at stop(), which billed menus, idle
// waits and the boot auto-resume as reading. These tests pin down the
// replacement policy against a hand-driven clock.

#include <gtest/gtest.h>

#include "nous/ReadingActivityTracker.h"

using namespace microreader;

namespace {

constexpr uint32_t kIdle = ReadingActivityTracker::kIdleTimeoutMs;  // 3 min

// Drive the tracker forward as the reader would: one advance() per frame.
void run_idle(ReadingActivityTracker& t, uint32_t& now, uint32_t duration, uint32_t step = 50) {
  const uint32_t end = now + duration;
  while (now < end) {
    now += step;
    t.advance(now);
  }
}

}  // namespace

// --- Manual open ------------------------------------------------------------

// The press that picked the book is consumed by the menu, so the reader sees no
// input at all in this scenario. It must still count: choosing the book is the
// statement of intent.
TEST(ReadingActivityTracker, ManualOpenCountsWithoutAnyReaderInput) {
  ReadingActivityTracker t;
  uint32_t now = 1000;
  t.on_open(now, OpenReason::UserOpened);
  EXPECT_TRUE(t.confirmed());

  run_idle(t, now, 60000);  // read the first page for a minute
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), 60000u);
}

TEST(ReadingActivityTracker, ManualOpenConfirmsExactlyOnce) {
  ReadingActivityTracker t;
  t.on_open(500, OpenReason::UserOpened);
  EXPECT_TRUE(t.consume_confirmation());
  EXPECT_FALSE(t.consume_confirmation());
}

// --- Boot auto-resume -------------------------------------------------------

TEST(ReadingActivityTracker, AutoResumeWithNoInputCountsNothing) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::AutoResumed);
  EXPECT_FALSE(t.confirmed());
  EXPECT_FALSE(t.consume_confirmation());

  run_idle(t, now, 9 * 60000);
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), 0u);
}

// Pressing Back to escape a book the device opened by itself is not reading.
TEST(ReadingActivityTracker, AutoResumeNotConfirmedByNonNavigationInput) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::AutoResumed);

  run_idle(t, now, 9 * 60000);
  t.on_activity(now, Activity::Other);  // Back / Power / options
  EXPECT_FALSE(t.confirmed());
  EXPECT_FALSE(t.consume_confirmation());
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), 0u);
}

// A page turn proves someone is reading — but only from that moment on. The
// idle stretch before it must not be paid for retroactively.
TEST(ReadingActivityTracker, AutoResumeConfirmsOnPageTurnAndCountsForwardOnly) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::AutoResumed);

  run_idle(t, now, 9 * 60000);
  t.on_activity(now, Activity::PageNavigation);
  EXPECT_TRUE(t.confirmed());
  EXPECT_TRUE(t.consume_confirmation());
  EXPECT_EQ(t.total_ms(), 0u) << "the nine idle minutes must not be credited";

  run_idle(t, now, 30000);
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), 30000u);
}

// --- Idle cutoff ------------------------------------------------------------

TEST(ReadingActivityTracker, StopsAccruingAfterIdleTimeout) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::UserOpened);

  run_idle(t, now, 30 * 60000);  // book left open for half an hour
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), kIdle) << "capped at the idle window, not 30 minutes";
}

TEST(ReadingActivityTracker, ActivityReopensTheWindow) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::UserOpened);

  run_idle(t, now, 10 * 60000);            // idle out (credits kIdle)
  t.on_activity(now, Activity::PageNavigation);
  run_idle(t, now, 60000);                 // a fresh minute of reading
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), kIdle + 60000u);
}

// A single long frame straddling the cutoff should still earn its valid part.
TEST(ReadingActivityTracker, TickStraddlingCutoffCreditsOnlyPreCutoffPortion) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::UserOpened);

  now += kIdle - 1000;   // 1s of budget left
  t.advance(now);
  now += 10000;          // one 10s frame
  t.advance(now);
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), kIdle) << "1s credited, the other 9s past the cutoff dropped";
}

// The order inside on_activity matters: extending the deadline before the
// preceding interval is evaluated would pay for the whole idle gap.
TEST(ReadingActivityTracker, ActivityAfterLongIdleDoesNotBackfill) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::UserOpened);

  now += 60 * 60000;  // an hour away from the device, no frames in between
  t.on_activity(now, Activity::PageNavigation);
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), kIdle) << "only the idle window, never the full hour";
}

// --- Child screens ----------------------------------------------------------

TEST(ReadingActivityTracker, TimeInChildScreenIsNotCounted) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::UserOpened);

  run_idle(t, now, 10000);   // 10s reading
  t.on_pause(now);
  now += 5 * 60000;          // 5 min in the options menu
  t.on_resume(now);
  run_idle(t, now, 10000);   // 10s more reading
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), 20000u);
}

TEST(ReadingActivityTracker, ResumeRefreshesWindowButDoesNotConfirm) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::AutoResumed);

  t.on_pause(now);
  now += 60000;
  t.on_resume(now);
  EXPECT_FALSE(t.confirmed()) << "dismissing a menu is not reading";

  run_idle(t, now, 60000);
  t.on_close(now);
  EXPECT_EQ(t.total_ms(), 0u);
}

// --- Clock edges ------------------------------------------------------------

TEST(ReadingActivityTracker, HandlesClockStartingAtZero) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::UserOpened);
  run_idle(t, now, 5000);
  t.on_close(now);
  EXPECT_EQ(t.total_ms(), 5000u);
}

// uint32_t millis() wraps every ~49.7 days; modular subtraction must carry us
// across the boundary without crediting a spurious 49 days.
TEST(ReadingActivityTracker, HandlesClockWrap) {
  ReadingActivityTracker t;
  uint32_t now = 0xFFFFF000u;
  t.on_open(now, OpenReason::UserOpened);

  for (int i = 0; i < 100; ++i) {  // 5s of frames, straddling the wrap
    now += 50;
    t.advance(now);
  }
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), 5000u);
}

TEST(ReadingActivityTracker, HeldNavigationKeepsCounting) {
  ReadingActivityTracker t;
  uint32_t now = 0;
  t.on_open(now, OpenReason::UserOpened);

  // Hold-to-page: an activity every frame for 10 minutes, well past the idle
  // window — all of it is real reading and must be credited.
  for (int i = 0; i < 12000; ++i) {
    now += 50;
    t.on_activity(now, Activity::PageNavigation);
  }
  t.on_close(now);

  EXPECT_EQ(t.total_ms(), 10u * 60000u);
}
