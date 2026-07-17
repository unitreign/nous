#pragma once

#include <cstdint>
#include <string>

#include "ListMenuScreen.h"

namespace microreader {

// Displays per-book reading statistics: times opened and total reading time.
// Populated by ReaderOptionsScreen before being pushed.
class StatsScreen final : public ListMenuScreen {
 public:
  StatsScreen() = default;

  void set_book_stats(const std::string& title, uint32_t times_opened, uint64_t reading_ms) {
    book_title_ = title;
    times_opened_ = times_opened;
    reading_ms_ = reading_ms;
  }

  const char* name() const override { return "Stats"; }

 protected:
  void on_start() override;
  void on_select(int /*index*/) override {}

 private:
  std::string book_title_;
  uint32_t times_opened_ = 0;
  uint64_t reading_ms_ = 0;
};

}  // namespace microreader
