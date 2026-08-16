#include "SeriesBookListScreen.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../Application.h"
#include "../content/BookIndex.h"

namespace microreader {

void SeriesBookListScreen::on_start() {
  title_ = series_name_.empty() ? "Uncategorized" : series_name_.c_str();
  entries_.clear();
  clear_items();

  force_chronicle_list_ = (ListMenuScreen::theme() == ListMenuScreen::MenuTheme::Lyra ||
                            ListMenuScreen::theme() == ListMenuScreen::MenuTheme::LyraExt);

  const auto& idx = BookIndex::instance();
  const StringPool& pool = idx.pool();

  // Prefix to strip from display titles when inside a named series.
  const std::string prefix = series_name_.empty() ? "" : series_name_ + ": ";

  for (const auto& e : idx.entries()) {
    auto sv = e.series.view(pool);
    const bool match = series_name_.empty() ? sv.empty() : (sv == series_name_);
    if (!match) continue;

    BookEntry be;
    be.path = e.path.to_string(pool);
    be.title = std::string(e.title.view(pool));
    be.author = std::string(e.author.view(pool));
    be.read_time_ms = e.read_time_ms;
    be.series_index = e.series_index;

    // Strip "Series Name: " prefix from the displayed title — redundant in context.
    if (!prefix.empty() && be.title.size() > prefix.size() &&
        be.title.compare(0, prefix.size(), prefix) == 0)
      be.display_title = be.title.substr(prefix.size());
    else
      be.display_title = be.title;

    entries_.push_back(std::move(be));
  }

  // Sort: primary by series_index (volume number), secondary natural sort by title.
  // Natural sort handles numeric segments correctly (1, 2, 10 not 1, 10, 2).
  auto nat_less_str = [](const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
      const bool an = std::isdigit(static_cast<unsigned char>(a[i]));
      const bool bn = std::isdigit(static_cast<unsigned char>(b[j]));
      if (an && bn) {
        const unsigned long av = std::strtoul(a.c_str() + i, nullptr, 10);
        const unsigned long bv = std::strtoul(b.c_str() + j, nullptr, 10);
        if (av != bv) return av < bv;
        while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) ++i;
        while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
      } else {
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[j]));
        if (ca != cb) return ca < cb;
        ++i; ++j;
      }
    }
    return a.size() < b.size();
  };

  if (entries_.size() > 1) {
    std::sort(entries_.begin(), entries_.end(), [&nat_less_str](const BookEntry& a, const BookEntry& b) {
      if (a.series_index != b.series_index) return a.series_index < b.series_index;
      return nat_less_str(a.title, b.title);
    });
  }

  // Build item list after sort.
  for (const auto& be : entries_)
    add_item(be.display_title);

  if (entries_.empty())
    add_item("No books");
}

std::string SeriesBookListScreen::nous_header_left() const {
  const int n = static_cast<int>(entries_.size());
  char tmp[24];
  std::snprintf(tmp, sizeof(tmp), n == 1 ? "1 BOOK" : "%d BOOKS", n);
  return tmp;
}

std::string_view SeriesBookListScreen::get_item_subtitle(int index) const {
  if (index < 0 || index >= static_cast<int>(entries_.size())) return {};
  subtitle_buf_ = entries_[index].author;
  return subtitle_buf_;
}

std::string_view SeriesBookListScreen::get_item_right(int index) const {
  if (index < 0 || index >= static_cast<int>(entries_.size())) return {};
  const uint64_t ms = entries_[index].read_time_ms;
  if (ms >= 60000) {
    const uint64_t total_min = ms / 60000;
    const unsigned hours = static_cast<unsigned>(total_min / 60);
    const unsigned mins  = static_cast<unsigned>(total_min % 60);
    char tmp[16];
    if (hours > 0)
      std::snprintf(tmp, sizeof(tmp), "%uh %um", hours, mins);
    else
      std::snprintf(tmp, sizeof(tmp), "%um", mins);
    right_buf_ = tmp;
  } else {
    right_buf_ = "\xe2\x80\x93";  // en dash –
  }
  return right_buf_;
}

void SeriesBookListScreen::on_back() {
  if (app_) app_->pop_screen();
}

void SeriesBookListScreen::on_select(int index) {
  if (!app_ || index < 0 || index >= static_cast<int>(entries_.size())) return;
  show_opening_indicator();
  app_->record_book_opened(entries_[index].path);
  app_->reader()->set_path(entries_[index].path.c_str());
  app_->push_screen(ScreenId::Reader);
}

}  // namespace microreader
