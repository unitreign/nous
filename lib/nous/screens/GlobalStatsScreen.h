#pragma once

#include <cstdint>
#include <string>

#include "ListMenuScreen.h"

namespace microreader {

class GlobalStatsScreen final : public ListMenuScreen {
 public:
  GlobalStatsScreen() = default;

  const char* name() const override { return "Library Stats"; }

  int count() const override { return 0; }
  std::string_view get_item_label(int) const override { return {}; }

  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;
  int get_visible_count_(int, int) const override { return 0; }

 protected:
  void on_start() override;
  void on_select(int) override {}
  void on_back() override;

 private:
  // Cached stats computed in on_start()
  int total_books_    = 0;
  int started_books_  = 0;
  int finished_books_ = 0;
  uint64_t total_read_ms_    = 0;
  uint32_t total_page_turns_ = 0;
  uint32_t total_sessions_   = 0;

  std::string most_read_title_;
  std::string most_read_author_;
  uint64_t most_read_ms_    = 0;
  uint32_t most_read_opens_ = 0;
  uint16_t most_read_pct_   = 0;

  BitmapFont title_font_;
};

}  // namespace microreader
