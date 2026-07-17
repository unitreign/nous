#include "ConvertAllScreen.h"

#include <algorithm>
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

void ConvertAllScreen::scan_jobs_() {
  jobs_.clear();
  if (!app_ || !app_->data_dir_) return;

  // MainMenu::stop() (called via pause() default) clears BookIndex entries.
  // Reload from disk so we see all books even when navigated here from Settings.
  if (BookIndex::instance().entries().empty()) {
    std::string index_path = std::string(app_->data_dir_) + "/book_index.dat";
    BookIndex::instance().load(index_path);
  }

  const StringPool& pool = BookIndex::instance().pool();
  for (const auto& e : BookIndex::instance().entries()) {
    BookJob job;
    job.path = std::string(e.path.view(pool));
    job.title = std::string(e.title.view(pool));
    std::string stem = derive_stem_(job.path);
    job.mrb_path = std::string(app_->data_dir_) + "/cache/" + stem + "/book.mrb";
    FILE* f = std::fopen(job.mrb_path.c_str(), "rb");
    if (f) {
      std::fclose(f);
      job.done = true;
      job.skipped = true;
    }
    jobs_.push_back(std::move(job));
  }
}

void ConvertAllScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  current_idx_ = 0;
  converted_count_ = 0;
  failed_count_ = 0;
  cancel_requested_ = false;
  phase_ = Phase::Converting;

  scan_jobs_();

  int pending = static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(),
      [](const BookJob& j) { return !j.skipped; }));

  if (pending == 0) {
    phase_ = Phase::Done;
    draw_done_(buf);
    buf.full_refresh();
    return;
  }

  buf.fill(true);
  buf.full_refresh();
}

void ConvertAllScreen::stop() {
  jobs_.clear();
  buf_ = nullptr;
}

void ConvertAllScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;

  Button btn;
  while (buttons.next_press(btn)) {
    if (btn == Button::Button0) {
      if (phase_ == Phase::Done) {
        app_->pop_screen();
        return;
      }
      cancel_requested_ = true;
    }
  }

  if (phase_ == Phase::Done) return;

  // Advance to next un-converted job.
  while (current_idx_ < static_cast<int>(jobs_.size()) && jobs_[current_idx_].done)
    ++current_idx_;

  if (cancel_requested_ || current_idx_ >= static_cast<int>(jobs_.size())) {
    phase_ = Phase::Done;
    draw_done_(buf);
    buf.full_refresh();
    return;
  }

  BookJob& job = jobs_[current_idx_];
  const int total_pending = static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(),
      [](const BookJob& j) { return !j.skipped; }));
  const int job_num = converted_count_ + failed_count_ + 1;

  // Create per-book cache directory.
  std::string cache_dir = job.mrb_path.substr(0, job.mrb_path.rfind('/'));
#ifdef ESP_PLATFORM
  mkdir(cache_dir.c_str(), 0775);
#else
  try { std::filesystem::create_directories(cache_dir); } catch (...) {}
#endif

  buf.sync_bw_ram();
  {
    char msg[48];
    std::snprintf(msg, sizeof(msg), "Book %d / %d", job_num, total_pending);
    buf.show_loading(msg, 0);
  }

  Book book;
  auto err = book.open(job.path.c_str(), buf.scratch_buf1(), buf.scratch_buf2());
  bool ok = false;
  if (err == EpubError::Ok && book.chapter_count() > 0) {
    ok = convert_epub_to_mrb_streaming(
        book, job.mrb_path.c_str(), buf.scratch_buf1(), buf.scratch_buf2(),
        [&buf, job_num, total_pending](int done, int total) {
          int pct = total > 0 ? done * 100 / total : 0;
          char msg[48];
          std::snprintf(msg, sizeof(msg), "Book %d / %d", job_num, total_pending);
          buf.show_loading(msg, pct);
        });
  }
  book.close();
  buf.reset_after_scratch(true);

  if (ok) {
    job.done = true;
    ++converted_count_;
  } else {
    job.done = true;
    job.failed = true;
    ++failed_count_;
    std::remove(job.mrb_path.c_str());
  }

  ++current_idx_;

  bool all_done = std::all_of(jobs_.begin(), jobs_.end(), [](const BookJob& j) { return j.done; });
  if (all_done) {
    phase_ = Phase::Done;
    draw_done_(buf);
    buf.full_refresh();
  }
}

void ConvertAllScreen::draw_done_(DrawBuffer& buf) const {
  buf.fill(true);
  const int W = buf.width();
  const int H = buf.height();
  const int cx = W / 2;

  buf.draw_text_centered(cx, H / 4, "Convert All", false);

  char line[48];
  if (cancel_requested_)
    std::snprintf(line, sizeof(line), "Stopped. %d converted.", converted_count_);
  else if (converted_count_ == 0 && failed_count_ == 0)
    std::snprintf(line, sizeof(line), "All books already converted.");
  else
    std::snprintf(line, sizeof(line), "Done. %d converted.", converted_count_);
  buf.draw_text_centered(cx, H / 2 - 10, line, false);

  if (failed_count_ > 0) {
    char fail[32];
    std::snprintf(fail, sizeof(fail), "%d failed.", failed_count_);
    buf.draw_text_centered(cx, H / 2 + 10, fail, false);
  }

  int skipped = static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(),
      [](const BookJob& j) { return j.skipped; }));
  if (skipped > 0) {
    char skip[48];
    std::snprintf(skip, sizeof(skip), "%d already converted.", skipped);
    buf.draw_text_centered(cx, H * 3 / 4, skip, false);
  }

  buf.draw_text_centered(cx, H - 30, "Back to return", false);
}

}  // namespace microreader
