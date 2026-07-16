#include "MainMenu.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/BookIndex.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

namespace microreader {

// Returns a view into `path` pointing at the bare filename without extension.
static std::string_view filename_sv(const std::string& path) {
  const char* name = path.c_str();
  const char* sep = std::strrchr(name, '/');
#ifdef _WIN32
  const char* bsep = std::strrchr(name, '\\');
  if (bsep && (!sep || bsep > sep))
    sep = bsep;
#endif
  if (sep)
    name = sep + 1;
  const char* dot = std::strrchr(name, '.');
  size_t len = dot ? static_cast<size_t>(dot - name) : std::strlen(name);
  return {name, len};
}

static bool ci_less(std::string_view a, std::string_view b) {
  size_t min_len = std::min(a.size(), b.size());
#ifdef _WIN32
  int cmp = _strnicmp(a.data(), b.data(), min_len);
#else
  int cmp = strncasecmp(a.data(), b.data(), min_len);
#endif
  if (cmp != 0) return cmp < 0;
  return a.size() < b.size();
}

void MainMenu::on_start() {
  title_ = "Nous";

  if (!app_->data_dir_) {
    needs_scan_ = false;
    return;
  }

  std::string index_path = std::string(app_->data_dir_) + "/book_index.dat";

  if (BookIndex::instance().load(index_path)) {
    populate_list_();
    needs_scan_ = false;
  } else {
    needs_scan_ = true;
  }
  cached_generation_ = BookIndex::instance().generation();
}

void MainMenu::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  // Long-press back (~3s) → hidden books; short press → Settings (on_back()).
  // While held we suppress Button0 from the forwarded state so ListMenuScreen
  // doesn't fire on_back() mid-hold. On release we decide which action to take.
  const bool back_down = buttons.is_down(Button::Button0);
  ButtonState fwd = buttons;
  if (back_down) {
    if (back_hold_frames_ <= kHiddenHoldFrames)
      back_hold_frames_++;
    // Strip Button0 from the copy so ListMenuScreen never sees the press.
    fwd.pressed_latch &= ~(1u << static_cast<uint8_t>(Button::Button0));
    uint8_t nc = 0;
    for (uint8_t i = 0; i < fwd.press_history_count; ++i)
      if (static_cast<Button>(fwd.press_history[i]) != Button::Button0)
        fwd.press_history[nc++] = fwd.press_history[i];
    fwd.press_history_count = nc;
    back_was_down_ = true;
  } else if (back_was_down_) {
    back_was_down_ = false;
    const int held = back_hold_frames_;
    back_hold_frames_ = 0;
    if (held >= kHiddenHoldFrames) {
      if (app_) app_->push_screen(ScreenId::HiddenBooks);
    } else {
      on_back();
    }
    return;
  }

  // Detect external mutations (serial upload/delete/rename) while this screen
  // is visible. The generation counter is bumped by BookIndex on every
  // mutation that changes the logical contents.
  if (cached_generation_ != BookIndex::instance().generation()) {
    cached_generation_ = BookIndex::instance().generation();
    populate_list_();
    draw_all_(buf, runtime.battery_percentage());
    buf.full_refresh();
  }

  if (needs_scan_) {
    needs_scan_ = false;
    scan_directory_(buf);
    populate_list_();

    draw_all_(buf, runtime.battery_percentage());
    buf.full_refresh();
    cached_generation_ = BookIndex::instance().generation();
  }

  ListMenuScreen::update(fwd, buf, runtime);
}

void MainMenu::on_select(int index) {
  if (is_separator(index)) return;
  int real = entries_index_for(index);
  last_selected_path_ = entries_[real].path;
  app_->record_book_opened(entries_[real].path);
  app_->reader()->set_path(entries_[real].path.c_str());
  app_->push_screen(ScreenId::Reader);
}

void MainMenu::stop() {
  const std::string& cur = current_book_path();
  if (!cur.empty()) {
    initial_selection_ = cur;
    last_selected_path_ = cur;
  }

  { std::vector<BookEntry> tmp; entries_.swap(tmp); }
  free_items_storage();
  BookIndex::instance().clear_entries();
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

  buf.reset_after_scratch(true);
}

int MainMenu::count() const {
  return static_cast<int>(entries_.size()) + static_cast<int>(separators_.size());
}

bool MainMenu::is_separator(int index) const {
  for (const auto& s : separators_)
    if (s.first == index) return true;
  return false;
}

std::string_view MainMenu::get_item_label(int index) const {
  if (is_separator(index)) {
    for (const auto& s : separators_)
      if (s.first == index) return s.second;
    return {};
  }
  int real = entries_index_for(index);
  if (real < 0 || real >= static_cast<int>(entries_.size()))
    return {};
  const StringPool& pool = BookIndex::instance().pool();
  const BookEntry& e = entries_[real];
  const bool star = e.mrb_exists;
  const auto t = ListMenuScreen::theme();
  const bool chronicle = (t == ListMenuScreen::MenuTheme::Chronicle);
  const bool stele     = (t == ListMenuScreen::MenuTheme::Stele);

  auto title_sv  = e.title_ref.view(pool);
  auto fname_sv  = filename_sv(e.path);

  if (list_format_ == BookListFormat::TitleOnly) {
    if (!star) return title_sv;
    label_buf_ = chronicle ? std::string(title_sv) + " \xc2\xb7"
               : stele     ? "\xc2\xb7 " + std::string(title_sv)
               :              "* " + std::string(title_sv);
    return label_buf_;
  } else if (list_format_ == BookListFormat::Filename) {
    if (!star) return fname_sv;
    label_buf_ = chronicle ? std::string(fname_sv) + " \xc2\xb7"
               : stele     ? "\xc2\xb7 " + std::string(fname_sv)
               :              "* " + std::string(fname_sv);
    return label_buf_;
  } else {
    label_buf_ = std::string(title_sv) + " - " + std::string(e.author_ref.view(pool));
    if (star && !chronicle && !stele) label_buf_ = "* " + label_buf_;
    return std::string_view(label_buf_);
  }
}

std::string_view MainMenu::get_item_subtitle(int index) const {
  if (is_separator(index)) return {};
  int real = entries_index_for(index);
  if (real < 0 || real >= static_cast<int>(entries_.size())) return {};
  const BookEntry& e = entries_[real];
  const StringPool& pool = BookIndex::instance().pool();

  subtitle_buf_ = e.author_ref.view(pool);

  if (ListMenuScreen::theme() == ListMenuScreen::MenuTheme::Stele && e.read_time_ms >= 60000) {
    const uint64_t total_min = e.read_time_ms / 60000;
    const unsigned hours = static_cast<unsigned>(total_min / 60);
    const unsigned mins  = static_cast<unsigned>(total_min % 60);
    char tbuf[20];
    if (hours > 0)
      std::snprintf(tbuf, sizeof(tbuf), " \xc2\xb7 %uh %um", hours, mins);
    else
      std::snprintf(tbuf, sizeof(tbuf), " \xc2\xb7 %um", mins);
    subtitle_buf_ += tbuf;
  }
  return subtitle_buf_;
}

std::string MainMenu::nous_header_left() const {
  const int n = static_cast<int>(entries_.size());
  char buf[24];
  if (ListMenuScreen::theme() == ListMenuScreen::MenuTheme::Stele)
    std::snprintf(buf, sizeof(buf), n == 1 ? "1 book" : "%d books", n);
  else
    std::snprintf(buf, sizeof(buf), n == 1 ? "1 BOOK" : "%d BOOKS", n);
  return buf;
}

std::string_view MainMenu::get_item_right(int index) const {
  if (is_separator(index)) return {};
  int real = entries_index_for(index);
  if (real < 0 || real >= static_cast<int>(entries_.size())) return {};
  const BookEntry& e = entries_[real];

  if (e.read_time_ms >= 60000) {
    const uint64_t total_min = e.read_time_ms / 60000;
    const unsigned hours = static_cast<unsigned>(total_min / 60);
    const unsigned mins  = static_cast<unsigned>(total_min % 60);
    char tbuf[16];
    if (hours > 0)
      std::snprintf(tbuf, sizeof(tbuf), "%uh %um", hours, mins);
    else
      std::snprintf(tbuf, sizeof(tbuf), "%um", mins);
    right_buf_ = tbuf;
  } else {
    right_buf_ = "\xe2\x80\x93";  // en dash –
  }
  return right_buf_;
}

void MainMenu::populate_list_() {
  clear_items();
  entries_.clear();
  separators_.clear();

  const StringPool& bpool = BookIndex::instance().pool();
  const bool check_mrb = app_ && app_->data_dir_ && app_->show_converted_indicator();
  for (const auto& idx : BookIndex::instance().entries()) {
    BookEntry e;
    e.path = idx.path.to_string(bpool);
    e.title_ref = idx.title;
    e.author_ref = idx.author;
    e.last_open_order = idx.last_open_order;
    e.read_time_ms = idx.read_time_ms;
    if (check_mrb) {
      const char* name = e.path.c_str();
      const char* sep = std::strrchr(name, '/');
      if (sep) name = sep + 1;
      const char* dot = std::strrchr(name, '.');
      std::string stem(name, dot ? static_cast<size_t>(dot - name) : std::strlen(name));
      std::string mrb_path = std::string(app_->data_dir_) + "/cache/" + stem + "/book.mrb";
      FILE* mf = std::fopen(mrb_path.c_str(), "rb");
      if (mf) { std::fclose(mf); e.mrb_exists = true; }
    }
    entries_.push_back(std::move(e));
  }

  if (sort_order_ == BookSortOrder::LastOpened) {
    const auto fmt = list_format_;
    std::stable_sort(entries_.begin(), entries_.end(),
                     [&bpool, fmt](const BookEntry& a, const BookEntry& b) {
                      if (a.last_open_order != b.last_open_order)
                        return a.last_open_order > b.last_open_order;
                      if (fmt == BookListFormat::Filename)
                        return ci_less(filename_sv(a.path), filename_sv(b.path));
                      return ci_less(a.title_ref.view(bpool), b.title_ref.view(bpool));
                     });
    // Find where recent books end (first entry never opened)
    int split = static_cast<int>(entries_.size());
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
      if (entries_[i].last_open_order == 0) { split = i; break; }
    }
    if (split > 0 && split < static_cast<int>(entries_.size())) {
      // Anonymous divider line between recent and never-opened books
      separators_.push_back({split, ""});
    }
  } else if (list_format_ == BookListFormat::Filename) {
    std::stable_sort(entries_.begin(), entries_.end(),
                      [](const BookEntry& a, const BookEntry& b) { return ci_less(filename_sv(a.path), filename_sv(b.path)); });
  } else {
    std::stable_sort(entries_.begin(), entries_.end(),
                     [&bpool](const BookEntry& a, const BookEntry& b) {
                        return ci_less(a.title_ref.view(bpool), b.title_ref.view(bpool));
                     });
  }

  if (!initial_selection_.empty()) {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
      if (entries_[i].path == initial_selection_) {
        set_selected(visual_for_entries(i));
        break;
      }
    }
    initial_selection_.clear();
  }
}

}  // namespace microreader
