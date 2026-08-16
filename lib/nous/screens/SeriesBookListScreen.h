#pragma once

#include <string>
#include <vector>

#include "ListMenuScreen.h"

namespace microreader {

// Shows the books belonging to a single series (or "Uncategorized" for books with no series tag).
// Design mirrors MainMenu: same row layout, subtitle (author), right column (read time), tooltips.
class SeriesBookListScreen final : public ListMenuScreen {
 public:
  SeriesBookListScreen() = default;

  // Set the series to show before pushing this screen. Empty string = Uncategorized.
  void set_series(const std::string& series_name) { series_name_ = series_name; }

  const char* name() const override { return "Series Books"; }

  std::string_view get_item_subtitle(int index) const override;
  std::string_view get_item_right(int index) const override;
  std::string nous_header_left() const override;

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override;

 private:
  struct BookEntry {
    std::string path;
    std::string title;         // full title (used for sort tiebreaker)
    std::string display_title; // series prefix stripped (shown in list)
    std::string author;
    uint64_t read_time_ms = 0;
    float series_index = 0.0f;
  };
  std::string series_name_;
  std::vector<BookEntry> entries_;
  mutable std::string subtitle_buf_;
  mutable std::string right_buf_;
};

}  // namespace microreader
