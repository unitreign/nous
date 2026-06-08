#include "MainMenu.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../HeapLog.h"
#include "../content/BookIndex.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

namespace microreader {

void MainMenu::on_start() {
  title_ = "Microreader";

  if (!app_->data_dir_) {
    needs_scan_ = false;
    return;
  }

  std::string index_path = std::string(app_->data_dir_) + "/book_index.dat";

  if (BookIndex::instance().load(index_path)) {
    populate_list_();
    needs_scan_ = false;
  } else {
    // We defer heavy scanning to update() so we don't trip hardware watchdog.
    needs_scan_ = true;
  }
}

void MainMenu::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  if (needs_scan_) {
    needs_scan_ = false;
    scan_directory_(buf);
    populate_list_();

    // Force a redraw and full refresh since the list contents completely changed
    draw_all_(buf, runtime.battery_percentage());
    buf.full_refresh();
  }

  ListMenuScreen::update(buttons, buf, runtime);
}

void MainMenu::on_select(int index) {
  last_selected_path_ = entries_[index].path;
  app_->record_book_opened(entries_[index].path);
  app_->reader()->set_path(entries_[index].path.c_str());
  app_->push_screen(ScreenId::Reader);
}

void MainMenu::on_back() {
  app_->push_screen(ScreenId::Settings);
}

void MainMenu::scan_directory_(DrawBuffer& buf) {
  if (!books_dir_ || !app_->data_dir_)
    return;

  std::string root_dir = books_dir_;
  const std::string index_path = std::string(app_->data_dir_) + "/book_index.dat";

  buf.sync_bw_ram();

  BookIndex::instance().build_index(root_dir, buf);
  BookIndex::instance().save(index_path);

  // Refresh to clean up the loading bar
  buf.reset_after_scratch(true);
}

void MainMenu::populate_list_() {
  clear_items();
  entries_.clear();

  const StringPool& bpool = BookIndex::instance().pool();
  for (const auto& index_entry : BookIndex::instance().entries()) {
    BookEntry e;
    e.path = index_entry.path.to_string(bpool);

    if (list_format_ == BookListFormat::TitleOnly) {
      e.label = index_entry.title.to_string(bpool);
    } else if (list_format_ == BookListFormat::Filename) {
      const char* name = e.path.c_str();
      const char* sep = std::strrchr(name, '/');
#ifdef _WIN32
      const char* bsep = std::strrchr(name, '\\');
      if (bsep && (!sep || bsep > sep))
        sep = bsep;
#endif
      if (sep)
        name = sep + 1;

      const char* dot = std::strrchr(name, '.');
      if (dot) {
        e.label = std::string(name, dot - name);
      } else {
        e.label = name;
      }
    } else {
      e.label = index_entry.title.to_string(bpool) + " - " + index_entry.author.to_string(bpool);  // Title & Author
    }

    entries_.push_back(std::move(e));
    entries_.back().last_open_order = index_entry.last_open_order;
  }

  // Sort the list according to sort_order_.
  // We need to re-sync items_ in ListMenuScreen after sorting, so we rebuild
  // it from scratch after the sort rather than calling add_item() before sort.
  if (sort_order_ == BookSortOrder::ByLastOpened) {
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const BookEntry& a, const BookEntry& b) { return a.last_open_order > b.last_open_order; });
  } else {
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const BookEntry& a, const BookEntry& b) { return a.label < b.label; });
  }
  // Rebuild the list-menu items after sort.
  clear_items();
  for (const auto& e : entries_)
    add_item(e.label);

  // Restore cursor to the saved path.
  if (!initial_selection_.empty()) {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
      if (entries_[i].path == initial_selection_) {
        set_selected(i);
        break;
      }
    }
    initial_selection_.clear();
  }
}

}  // namespace microreader
