#pragma once

#include <string>

#include "ListMenuScreen.h"

namespace microreader {

// Home-screen theme for Lyra.
// Shows 1 recent book (quick-open) and nav items: All Books, Recent Books, Settings.
// Long-press back → Hidden Books; short press → no-op (no parent screen).
class LyraScreen final : public ListMenuScreen {
 public:
  LyraScreen() = default;

  const char* name() const override { return "Home"; }

  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;
  int get_visible_count_(int H, int scroll_off) const override { return count(); }

  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override {}  // root screen — nowhere to go back to

 private:
  bool has_recent_ = false;
  std::string recent_title_;
  std::string recent_author_;
  std::string recent_path_;

  int idx_recent_      = -1;
  int idx_all_books_   = -1;
  int idx_recent_books_ = -1;
  int idx_settings_    = -1;

  static constexpr int kHiddenHoldFrames = 15;
  int back_hold_frames_ = 0;
  bool back_was_down_   = false;
};

}  // namespace microreader
