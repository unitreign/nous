#pragma once

#include <string>
#include <vector>

#include "ListMenuScreen.h"

namespace microreader {

// Shows the list of series found in the book index.
// Design mirrors MainMenu/RecentBooksScreen: same row layout, bottom tooltips, theme rendering.
class SeriesListScreen final : public ListMenuScreen {
 public:
  SeriesListScreen() = default;

  const char* name() const override { return "Series"; }

  std::string_view get_item_right(int index) const override;
  std::string nous_header_left() const override;

  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override;

 private:
  struct SeriesEntry {
    std::string name;
    int book_count = 0;
  };
  std::vector<SeriesEntry> entries_;
  mutable std::string right_buf_;
  bool needs_scan_ = false;

  void build_list_();
};

}  // namespace microreader
