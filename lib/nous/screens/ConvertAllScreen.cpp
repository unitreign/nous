#include "ConvertAllScreen.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/Book.h"
#include "../content/BookIndex.h"
#include "../content/mrb/MrbConverter.h"

#ifdef ESP_PLATFORM
#include <sys/stat.h>
#include <unistd.h>
#else
#include <filesystem>
#endif

namespace microreader {

// ---------------------------------------------------------------------------
// Natural (numeric-aware) sort
// ---------------------------------------------------------------------------

static bool natural_less(std::string_view a, std::string_view b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
      size_t si = i, sj = j;
      while (i < a.size() && std::isdigit((unsigned char)a[i])) ++i;
      while (j < b.size() && std::isdigit((unsigned char)b[j])) ++j;
      size_t la = i - si, lb = j - sj;
      if (la != lb) return la < lb;
      int cmp = std::strncmp(a.data() + si, b.data() + sj, la);
      if (cmp != 0) return cmp < 0;
    } else {
      char ca = (char)std::tolower((unsigned char)a[i++]);
      char cb = (char)std::tolower((unsigned char)b[j++]);
      if (ca != cb) return ca < cb;
    }
  }
  return (a.size() - i) < (b.size() - j);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string ConvertAllScreen::derive_stem_(const std::string& path) {
  const char* name = path.c_str();
  const char* sep = std::strrchr(name, '/');
#ifdef _WIN32
  const char* bsep = std::strrchr(name, '\\');
  if (bsep && (!sep || bsep > sep)) sep = bsep;
#endif
  if (sep) name = sep + 1;
  const char* dot = std::strrchr(name, '.');
  size_t len = dot ? static_cast<size_t>(dot - name) : std::strlen(name);
  return std::string(name, len);
}

int ConvertAllScreen::unconverted_count_() const {
  int n = 0;
  for (const auto& e : entries_)
    if (!e.converted) ++n;
  return n;
}

void ConvertAllScreen::scan_entries_() {
  entries_.clear();
  if (!app_ || !app_->data_dir_) return;

  const std::string index_path = std::string(app_->data_dir_) + "/book_index.dat";
  BookIndex::instance().load(index_path);

  const StringPool& pool = BookIndex::instance().pool();
  for (const auto& bk : BookIndex::instance().entries()) {
    Entry e;
    e.path  = std::string(bk.path.view(pool));
    e.title = std::string(bk.title.view(pool));
    if (e.title.empty()) e.title = derive_stem_(e.path);

    const std::string mrb_path   = std::string(app_->data_dir_) + "/cache/" +
                                   derive_stem_(e.path) + "/book.mrb";
    const std::string cover_path = cover_bin_path(e.path.c_str(), app_->data_dir_);

    FILE* mf = std::fopen(mrb_path.c_str(), "rb");
    if (mf) { std::fclose(mf); e.converted = true; }
    FILE* cf = std::fopen(cover_path.c_str(), "rb");
    if (cf) { std::fclose(cf); e.has_cover = true; }

    entries_.push_back(std::move(e));
  }

  std::stable_sort(entries_.begin(), entries_.end(),
      [](const Entry& a, const Entry& b) { return natural_less(a.title, b.title); });
}

void ConvertAllScreen::rebuild_items_() {
  clear_items();
  add_item("Convert All");
  for (const auto& e : entries_)
    add_item(e.title);
}

// ---------------------------------------------------------------------------
// ListMenuScreen overrides
// ---------------------------------------------------------------------------

void ConvertAllScreen::stop() {
  entries_.clear();
  entries_.shrink_to_fit();
}

void ConvertAllScreen::on_start() {
  title_ = "Convert Books";
  scan_entries_();
  rebuild_items_();
  force_chronicle_list_ = (ListMenuScreen::theme() == ListMenuScreen::MenuTheme::Lyra ||
                            ListMenuScreen::theme() == ListMenuScreen::MenuTheme::LyraExt);
}

void ConvertAllScreen::on_back() {
  if (app_) app_->pop_screen();
}

std::string_view ConvertAllScreen::get_item_subtitle(int index) const {
  if (index == 0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d unconverted", unconverted_count_());
    subtitle_buf_ = buf;
    return subtitle_buf_;
  }
  const int ei = index - 1;
  if (ei < 0 || ei >= static_cast<int>(entries_.size())) return {};
  const auto& e = entries_[ei];
  char sbuf[64];
  std::snprintf(sbuf, sizeof(sbuf), "%s  |  %s",
                e.converted ? "converted" : "not converted",
                e.has_cover ? "cover"     : "no cover");
  subtitle_buf_ = sbuf;
  return subtitle_buf_;
}

void ConvertAllScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  ListMenuScreen::draw_all_(buf, battery_pct);
  if (confirm_picker_.is_open())
    confirm_picker_.draw(buf);
}

void ConvertAllScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  cur_buf_     = &buf;
  cur_runtime_ = &runtime;

  if (confirm_picker_.is_open()) {
    if (confirm_picker_.update(buttons)) {
      draw_all_(buf, runtime.battery_percentage());
      if (confirm_picker_.is_open())
        buf.refresh();
      else
        buf.full_refresh();
    }
    cur_buf_     = nullptr;
    cur_runtime_ = nullptr;
    return;
  }

  ListMenuScreen::update(buttons, buf, runtime);
  cur_buf_     = nullptr;
  cur_runtime_ = nullptr;
}

void ConvertAllScreen::on_select(int index) {
  if (!cur_buf_ || !cur_runtime_) return;
  DrawBuffer& buf     = *cur_buf_;
  IRuntime&   runtime = *cur_runtime_;

  if (index == 0) {
    const int unc = unconverted_count_();
    if (unc == 0) return;
    char ptitle[64];
    std::snprintf(ptitle, sizeof(ptitle), "Convert All (%d unconverted)", unc);
    confirm_picker_.init(ui_font_);
    confirm_picker_.open(ptitle,
        {"Convert all (may take a long time)", "Cancel"}, 1,
        [this](int sel) {
          if (sel != 0 || !cur_buf_ || !cur_runtime_) return;
          struct Job { std::string path, title; };
          std::vector<Job> jobs;
          for (auto& e : entries_)
            if (!e.converted) jobs.push_back({e.path, e.title});
          entries_.clear();
          entries_.shrink_to_fit();
          for (auto& job : jobs)
            do_convert_path_(job.path, job.title, *cur_buf_, *cur_runtime_);
          app_->keep_awake();
          scan_entries_();
          rebuild_items_();
        });
    return;  // ListMenuScreen's needs_draw will call our draw_all_() which includes the picker
  }

  const int ei = index - 1;
  if (ei < 0 || ei >= static_cast<int>(entries_.size())) return;

  if (entries_[ei].converted) {
    do_delete_(ei, buf, runtime);
    scan_entries_();
  } else {
    const std::string path = entries_[ei].path;
    const std::string ttl  = entries_[ei].title;
    entries_.clear();
    entries_.shrink_to_fit();
    do_convert_path_(path, ttl, buf, runtime);
    app_->keep_awake();
    scan_entries_();
  }
  rebuild_items_();
  // ListMenuScreen's needs_draw path redraws and refreshes after on_select() returns.
}

// ---------------------------------------------------------------------------
// Convert / Delete
// ---------------------------------------------------------------------------

void ConvertAllScreen::do_convert_path_(const std::string& path, const std::string& title,
                                         DrawBuffer& buf, IRuntime& runtime) {
  if (!app_) return;

  const std::string stem       = derive_stem_(path);
  const std::string cache_dir  = std::string(app_->data_dir_) + "/cache/" + stem;
  const std::string mrb_path   = cache_dir + "/book.mrb";
  const std::string cover_path = cover_bin_path(path.c_str(), app_->data_dir_);
  const std::string sleep_path = cover_sleep_bin_path(path.c_str(), app_->data_dir_);

#ifdef ESP_PLATFORM
  mkdir(cache_dir.c_str(), 0775);
#else
  try { std::filesystem::create_directories(cache_dir); } catch (...) {}
#endif

  buf.sync_bw_ram();
  buf.show_loading(title.c_str(), 0);

  Book book;
  auto err = book.open(path.c_str(), buf.scratch_buf1(), buf.scratch_buf2());
  if (err == EpubError::Ok && book.chapter_count() > 0) {
    // Write covers while book is freshly open — BEFORE conversion trashes scratch bufs.
    runtime.yield();
    book.write_cover_bin(cover_path.c_str(), 160, 240, buf.scratch_buf1(), DrawBuffer::kBufSize);
    runtime.yield();
    if (app_->sleep_is_book_cover())
      book.write_cover_bin(sleep_path.c_str(), 480, 786, buf.scratch_buf1(), DrawBuffer::kBufSize);
    runtime.yield();

    convert_epub_to_mrb_streaming(
        book, mrb_path.c_str(), buf.scratch_buf1(), buf.scratch_buf2(),
        [&buf, &title, &runtime](int done, int total) {
          int pct = total > 0 ? done * 100 / total : 0;
          buf.show_loading(title.c_str(), pct);
          runtime.yield();
        });
  }
  book.close();
  buf.reset_after_scratch(true);
}

void ConvertAllScreen::do_delete_(int idx, DrawBuffer& buf, IRuntime& runtime) {
  if (!app_) return;
  const std::string path       = entries_[idx].path;
  const std::string stem       = derive_stem_(path);
  const std::string cache_dir  = std::string(app_->data_dir_) + "/cache/" + stem;
  const std::string mrb_path   = cache_dir + "/book.mrb";
  const std::string cover_path = cover_bin_path(path.c_str(), app_->data_dir_);
  const std::string sleep_path = cover_sleep_bin_path(path.c_str(), app_->data_dir_);

  buf.sync_bw_ram();
  buf.show_loading("Deleting...", 0);

  std::remove(mrb_path.c_str());
  std::remove(cover_path.c_str());
  std::remove(sleep_path.c_str());
#ifdef ESP_PLATFORM
  ::rmdir(cache_dir.c_str());
#else
  try { std::filesystem::remove(cache_dir); } catch (...) {}
#endif

  buf.show_loading("Deleting...", 100);
  for (int i = 0; i < 150; ++i) runtime.yield();
}

}  // namespace microreader
