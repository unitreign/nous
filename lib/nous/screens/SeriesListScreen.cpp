#include "SeriesListScreen.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "../Application.h"
#include "../content/BookIndex.h"

namespace microreader {

void SeriesListScreen::on_start() {
  needs_scan_ = false;

  if (!app_ || !app_->data_dir_) {
    build_list_();
    return;
  }

  auto& idx = BookIndex::instance();
  if (idx.entries().empty()) {
    const std::string idx_path = std::string(app_->data_dir_) + "/book_index.dat";
    idx.load(idx_path);
  }

  if (idx.scanned_count() < static_cast<uint32_t>(idx.entries().size())) {
    needs_scan_ = true;
    // Show placeholder until update() fires the scan.
    title_ = "Series";
    clear_items();
    add_item("Reading series info...");
    force_chronicle_list_ = (ListMenuScreen::theme() == ListMenuScreen::MenuTheme::Lyra ||
                              ListMenuScreen::theme() == ListMenuScreen::MenuTheme::LyraExt);
    return;
  }

  build_list_();
}

void SeriesListScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  if (needs_scan_) {
    needs_scan_ = false;
    const std::string idx_path = std::string(app_->data_dir_) + "/book_index.dat";
    BookIndex::instance().scan_series(idx_path, buf, false);
    build_list_();
    draw_all_(buf, runtime.battery_percentage());
    buf.full_refresh();
  }
  ListMenuScreen::update(buttons, buf, runtime);
}

void SeriesListScreen::build_list_() {
  title_ = "Series";
  entries_.clear();
  clear_items();

  force_chronicle_list_ = (ListMenuScreen::theme() == ListMenuScreen::MenuTheme::Lyra ||
                            ListMenuScreen::theme() == ListMenuScreen::MenuTheme::LyraExt);

  const auto& idx = BookIndex::instance();
  const StringPool& pool = idx.pool();

  std::map<std::string, int> counts;
  for (const auto& e : idx.entries()) {
    auto sv = e.series.view(pool);
    std::string key = sv.empty() ? "" : std::string(sv);
    counts[key]++;
  }

  std::vector<SeriesEntry> named;
  int uncategorized = 0;
  for (auto& [name, cnt] : counts) {
    if (name.empty())
      uncategorized = cnt;
    else
      named.push_back({name, cnt});
  }

  auto nat_less = [](const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
      const bool an = std::isdigit(static_cast<unsigned char>(a[i]));
      const bool bn = std::isdigit(static_cast<unsigned char>(b[j]));
      if (an && bn) {
        // compare numeric segments by value
        const unsigned long av = std::strtoul(a.c_str() + i, nullptr, 10);
        const unsigned long bv = std::strtoul(b.c_str() + j, nullptr, 10);
        if (av != bv) return av < bv;
        // skip digits
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
  std::sort(named.begin(), named.end(), [&nat_less](const SeriesEntry& a, const SeriesEntry& b) {
    return nat_less(a.name, b.name);
  });

  for (auto& se : named) {
    add_item(se.name);
    entries_.push_back(std::move(se));
  }

  if (uncategorized > 0) {
    add_item("Uncategorized");
    entries_.push_back({"", uncategorized});
  }

  if (entries_.empty())
    add_item("No series found");
}

std::string SeriesListScreen::nous_header_left() const {
  const int n = static_cast<int>(entries_.size());
  char tmp[32];
  std::snprintf(tmp, sizeof(tmp), n == 1 ? "1 SERIES" : "%d SERIES", n);
  return tmp;
}

std::string_view SeriesListScreen::get_item_right(int index) const {
  if (index < 0 || index >= static_cast<int>(entries_.size())) return {};
  const int cnt = entries_[index].book_count;
  char tmp[16];
  std::snprintf(tmp, sizeof(tmp), cnt == 1 ? "1 book" : "%d books", cnt);
  right_buf_ = tmp;
  return right_buf_;
}

void SeriesListScreen::on_back() {
  if (app_) app_->pop_screen();
}

void SeriesListScreen::on_select(int index) {
  if (!app_ || index < 0 || index >= static_cast<int>(entries_.size())) return;
  app_->series_book_list_screen()->set_series(entries_[index].name);
  app_->push_screen(ScreenId::SeriesBookList);
}

}  // namespace microreader
