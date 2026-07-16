#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#include "../Input.h"
#include "../content/StringPool.h"
#include "../display/DrawBuffer.h"
#include "ListMenuScreen.h"

namespace microreader {

enum class BookListFormat { TitleOnly, TitleAndAuthor, Filename };
enum class BookSortOrder { Alphabetical, LastOpened };

// Main screen — lists EPUB books from a directory.
// Button1 = open book, Button0 = settings.
class MainMenu final : public ListMenuScreen {
 public:
  MainMenu() = default;

  void set_books_dir(const char* dir) {
    books_dir_ = dir;
  }

  // Restore the book list selection to the entry matching this path.
  // Call before start(); applied after directory scan.
  void set_initial_selection(const char* path) {
    initial_selection_ = path ? path : "";
  }

  // The full path of the most recently selected (opened) book.
  const std::string& last_selected_book_path() const {
    return last_selected_path_;
  }

  // The full path of the currently highlighted entry (even if not yet opened).
  const std::string& current_book_path() const {
    int idx = entries_index_for(selected());
    if (idx >= 0 && idx < static_cast<int>(entries_.size()))
      return entries_[idx].path;
    static const std::string kEmpty;
    return kEmpty;
  }

  bool has_books_dir() const {
    return books_dir_ != nullptr;
  }

  const char* books_dir() const {
    return books_dir_;
  }

  const char* name() const override {
    return "Books";
  }

  BookListFormat list_format() const {
    return list_format_;
  }
  void set_list_format(BookListFormat format) {
    list_format_ = format;
  }

  BookSortOrder sort_order() const {
    return sort_order_;
  }
  void set_sort_order(BookSortOrder order) {
    sort_order_ = order;
  }

  void set_app(Application* app) {
    app_ = app;
  }

  std::string_view get_item_label(int index) const override;
  bool is_separator(int index) const override;
  int count() const override;

  void start(DrawBuffer& buf, IRuntime& runtime) override {
    buf_ = &buf;
    ListMenuScreen::start(buf, runtime);
  }

  void stop() override;

  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override;

 private:
  const char* books_dir_ = nullptr;
  std::string initial_selection_;   // path to pre-select after scan
  std::string last_selected_path_;  // path of the most recently opened book
  DrawBuffer* buf_ = nullptr;
  BookListFormat list_format_ = BookListFormat::TitleOnly;
  BookSortOrder sort_order_ = BookSortOrder::Alphabetical;
  bool needs_scan_ = false;
  // Cached BookIndex::generation() value from the last populate_list_(). When
  // update() detects a mismatch, the index was mutated externally (e.g. by a
  // serial upload/delete/rename while this screen is showing) and we refresh
  // the list in place without requiring the user to navigate away and back.
  uint64_t cached_generation_ = 0;

  struct BookEntry {
    std::string path;
    StringRef title_ref;
    StringRef author_ref;
    uint32_t last_open_order = 0;
    bool mrb_exists = false;
  };
  std::vector<BookEntry> entries_;
  mutable std::string label_buf_;
  int separator_visual_index_ = -1;

  int entries_index_for(int visual) const {
    if (separator_visual_index_ < 0 || visual < separator_visual_index_) return visual;
    return visual - 1;
  }

  int visual_for_entries(int real) const {
    if (separator_visual_index_ < 0 || real < separator_visual_index_) return real;
    return real + 1;
  }

  void scan_directory_(DrawBuffer& buf);
  void populate_list_();

  // Back-button long-press state for hidden books gesture.
  // Frames held; on release, short=Settings, long=HiddenBooks.
  static constexpr int kHiddenHoldFrames = 15;  // ~3s at typical e-ink frame rate
  int back_hold_frames_ = 0;
  bool back_was_down_ = false;
};

}  // namespace microreader
