#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ListMenuScreen.h"

namespace microreader {

// Home-screen theme for Lyra Extended Like.
// Shows up to 3 recent books (covers + titles) and nav items.
// Long-press back → Hidden Books; short press → no-op.
class LyraExtScreen final : public ListMenuScreen {
 public:
  LyraExtScreen() = default;

  const char* name() const override { return "Home"; }

  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;
  int get_visible_count_(int H, int scroll_off) const override { return count(); }

  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override {}

 private:
  static constexpr int kMaxBooks = 3;

  struct BookSlot {
    std::string path;
    std::string title;
    std::vector<uint8_t> cover_data;
    uint16_t cover_w = 0, cover_h = 0;
    bool cover_loaded = false;
    bool cover_needs_extract = false;
    std::string bin_path;
  };

  BookSlot slots_[kMaxBooks];
  int num_books_ = 0;

  int idx_books_[kMaxBooks]  = {-1, -1, -1};
  int idx_all_books_         = -1;
  int idx_recent_books_      = -1;
  int idx_stats_             = -1;
  int idx_settings_          = -1;

  static constexpr int kHiddenHoldFrames = 15;
  int  back_hold_frames_ = 0;
  bool back_was_down_    = false;

  void load_cover_(int slot_idx);
};

}  // namespace microreader
