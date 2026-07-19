#include "RecentBooksScreen.h"

#include <algorithm>
#include <string>

#include "../Application.h"
#include "../content/BookIndex.h"

namespace microreader {

void RecentBooksScreen::on_start() {
  title_ = "Recent Books";
  entries_.clear();
  clear_items();

  if (app_ && app_->data_dir_) {
    const std::string idx_path = std::string(app_->data_dir_) + "/book_index.dat";
    BookIndex::instance().load(idx_path);
  }

  const StringPool& pool = BookIndex::instance().pool();

  struct Raw { uint32_t order; Entry e; };
  std::vector<Raw> raw;
  for (const auto& idx : BookIndex::instance().entries()) {
    if (idx.last_open_order == 0) continue;
    raw.push_back({idx.last_open_order, {
      idx.path.to_string(pool),
      std::string(idx.title.view(pool)),
      std::string(idx.author.view(pool)),
    }});
  }

  std::sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) {
    return a.order > b.order;
  });

  for (auto& r : raw) {
    add_item(r.e.title);
    entries_.push_back(std::move(r.e));
  }

  if (entries_.empty())
    add_item("No recently opened books");
}

std::string_view RecentBooksScreen::get_item_subtitle(int index) const {
  if (index < 0 || index >= static_cast<int>(entries_.size())) return {};
  subtitle_buf_ = entries_[index].author;
  return subtitle_buf_;
}

void RecentBooksScreen::on_back() {
  if (app_) app_->pop_screen();
}

void RecentBooksScreen::on_select(int index) {
  if (!app_ || index < 0 || index >= static_cast<int>(entries_.size())) return;
  app_->record_book_opened(entries_[index].path);
  app_->reader()->set_path(entries_[index].path.c_str());
  app_->push_screen(ScreenId::Reader);
}

}  // namespace microreader
