#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ListMenuScreen.h"

namespace microreader {

// Displays per-book reading statistics.
// Populated by ReaderOptionsScreen before being pushed.
class StatsScreen final : public ListMenuScreen {
 public:
  StatsScreen() = default;

  void set_book_stats(const std::string& title, const std::string& author,
                      const std::string& chapter_title, int chapter_idx, int chapter_count,
                      uint32_t times_opened, uint64_t reading_ms,
                      int progress_pct, int chapter_progress_pct,
                      uint64_t time_left_ms, uint32_t page_turns,
                      const std::string& book_path) {
    book_title_           = title;
    author_               = author;
    chapter_title_        = chapter_title;
    chapter_idx_          = chapter_idx;
    chapter_count_        = chapter_count;
    times_opened_         = times_opened;
    reading_ms_           = reading_ms;
    progress_pct_         = progress_pct;
    chapter_progress_pct_ = chapter_progress_pct;
    time_left_ms_         = time_left_ms;
    page_turns_           = page_turns;
    book_path_            = book_path;
  }

  const char* name() const override { return "Stats"; }

  // Render stats layout (no tooltips), then full_refresh + deep_sleep.
  // Call this from Application::do_sleep_ after populating via set_book_stats().
  void draw_for_sleep(DrawBuffer& buf);

 protected:
  void on_start() override;
  void on_select(int /*index*/) override {}
  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;

 private:
  void draw_content_(DrawBuffer& buf) const;
  void load_cover_();

 private:
  // Book info
  std::string book_title_;
  std::string author_;
  std::string chapter_title_;
  int chapter_idx_  = 0;
  int chapter_count_ = 0;
  std::string book_path_;

  // Stats
  uint32_t times_opened_         = 0;
  uint64_t reading_ms_           = 0;
  int      progress_pct_         = 0;
  int      chapter_progress_pct_ = 0;
  uint64_t time_left_ms_         = 0;
  uint32_t page_turns_           = 0;

  // Cover image (1-bit packed, MSB first)
  std::vector<uint8_t> cover_data_;
  uint16_t cover_w_ = 0;
  uint16_t cover_h_ = 0;
  bool cover_loaded_ = false;

  // 24px font (not available as a standard ListMenuScreen member)
  BitmapFont title_font_;
};

}  // namespace microreader
