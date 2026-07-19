#pragma once

#include <string>
#include <vector>

#include "ListMenuScreen.h"

namespace microreader {

// Shows only books that have been opened at least once, sorted most-recent first.
// Accessed from the Lyra home screen via "Recent Books".
class RecentBooksScreen final : public ListMenuScreen {
 public:
  RecentBooksScreen() = default;

  const char* name() const override { return "Recent"; }

  std::string_view get_item_subtitle(int index) const override;

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override;

 private:
  struct Entry {
    std::string path;
    std::string title;
    std::string author;
  };
  std::vector<Entry> entries_;
  mutable std::string subtitle_buf_;
};

}  // namespace microreader
