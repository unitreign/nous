#include "StatsScreen.h"

#include <cstdio>

namespace microreader {

void StatsScreen::on_start() {
  title_ = "Statistics";

  if (!book_title_.empty()) {
    subtitle_ = book_title_.size() > 40 ? book_title_.substr(0, 37) + "..." : book_title_;
  }

  char buf[64];

  std::snprintf(buf, sizeof(buf), "Times Opened: %u", static_cast<unsigned>(times_opened_));
  add_item(std::string(buf));

  uint64_t total_minutes = reading_ms_ / 60000ULL;
  uint64_t hours = total_minutes / 60ULL;
  uint64_t minutes = total_minutes % 60ULL;
  if (hours > 0)
    std::snprintf(buf, sizeof(buf), "Reading Time: %uh %02um",
                  static_cast<unsigned>(hours), static_cast<unsigned>(minutes));
  else
    std::snprintf(buf, sizeof(buf), "Reading Time: %um", static_cast<unsigned>(minutes));
  add_item(std::string(buf));
}

}  // namespace microreader
