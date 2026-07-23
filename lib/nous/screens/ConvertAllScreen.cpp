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

  // Always reload from disk — a prior screen may have left entries populated
  // from a different context (e.g. filtered/sorted view from MainMenu).
  std::string index_path = std::string(app_->data_dir_) + "/book_index.dat";
  BookIndex::instance().load(index_path);

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
  cover_idx_ = 0;
  converted_count_ = 0;
  failed_count_ = 0;
  cancel_requested_ = false;
  phase_ = Phase::Converting;
  ListMenuScreen::apply_ui_font(ui_font_);

  scan_jobs_();

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

  // Prevent auto-sleep from firing mid-conversion and resetting the device.
  if (app_) app_->keep_awake();

  if (phase_ == Phase::Covers) {
    if (cancel_requested_ || cover_idx_ >= static_cast<int>(jobs_.size())) {
      phase_ = Phase::Done;
      draw_done_(buf);
      buf.full_refresh();
      return;
    }
    const BookJob& job = jobs_[cover_idx_];
    const int total = static_cast<int>(jobs_.size());
    char msg[96];
    const std::string& title = job.title.empty() ? job.path : job.title;
    std::snprintf(msg, sizeof(msg), "Covers %d / %d: %.55s", cover_idx_ + 1, total, title.c_str());
    buf.sync_bw_ram();
    buf.show_loading(msg, cover_idx_ * 100 / total);
    if (app_) app_->ensure_cover_bin(job.path, buf.scratch_buf1(), buf.scratch_buf2(), DrawBuffer::kBufSize);
    buf.reset_after_scratch(true);
    ++cover_idx_;
    if (cover_idx_ >= total) {
      phase_ = Phase::Done;
      draw_done_(buf);
      buf.full_refresh();
    }
    return;
  }

  // Phase::Converting — advance to next un-converted job.
  while (current_idx_ < static_cast<int>(jobs_.size()) && jobs_[current_idx_].done)
    ++current_idx_;

  if (cancel_requested_ || current_idx_ >= static_cast<int>(jobs_.size())) {
    // Conversions done (or cancelled); move on to cover extraction.
    phase_ = Phase::Covers;
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
    char msg[96];
    const std::string& title = job.title.empty() ? job.path : job.title;
    std::snprintf(msg, sizeof(msg), "%d / %d: %.60s", job_num, total_pending, title.c_str());
    buf.show_loading(msg, 0);
  }

  Book book;
  auto err = book.open(job.path.c_str(), buf.scratch_buf1(), buf.scratch_buf2());
  bool ok = false;
  if (err == EpubError::Ok && book.chapter_count() > 0) {
    const std::string& title = job.title.empty() ? job.path : job.title;
    ok = convert_epub_to_mrb_streaming(
        book, job.mrb_path.c_str(), buf.scratch_buf1(), buf.scratch_buf2(),
        [&buf, job_num, total_pending, &title](int done, int total) {
          int pct = total > 0 ? done * 100 / total : 0;
          char msg[96];
          std::snprintf(msg, sizeof(msg), "%d / %d: %.60s", job_num, total_pending, title.c_str());
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
}

void ConvertAllScreen::draw_done_(DrawBuffer& buf) const {
  buf.fill(true);
  const int W = buf.width();
  const int H = buf.height();
  const bool have_font = ui_font_.valid();
  const int fa = have_font ? ui_font_.y_advance() : 16;
  const int bl = have_font ? static_cast<int>(ui_font_.baseline()) : 12;
  constexpr int kPad = 8;
  constexpr int kLM = 14;

  auto draw_centered = [&](int y, const char* text) {
    if (!text || !*text) return;
    const size_t len = std::strlen(text);
    if (have_font) {
      const int tw = static_cast<int>(ui_font_.word_width(text, len, FontStyle::Regular));
      buf.draw_text_proportional((W - tw) / 2, y + bl, text, len, ui_font_, false);
    } else {
      buf.draw_text_centered(W / 2, y, text, true);
    }
  };
  auto draw_left = [&](int y, const char* text) {
    if (!text || !*text) return;
    const size_t len = std::strlen(text);
    if (have_font)
      buf.draw_text_proportional(kLM, y + bl, text, len, ui_font_, false);
    else
      buf.draw_text_centered(W / 2, y, text, true);
  };

  // Title
  const char* heading = cancel_requested_ ? "Stopped" : "Done";
  int y = kPad;
  draw_centered(y, heading);
  y += fa + kPad;
  buf.fill_rect(kLM, y, W - kLM * 2, 1, false);
  y += kPad;

  int skipped = static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(),
      [](const BookJob& j) { return j.skipped && !j.failed; }));

  if (!cancel_requested_ && converted_count_ == 0 && failed_count_ == 0) {
    draw_centered(H / 2 - fa / 2, "All books up to date.");
  } else {
    char line[64];
    std::snprintf(line, sizeof(line), "Converted:    %d", converted_count_);
    draw_left(y, line);
    y += fa + 4;

    std::snprintf(line, sizeof(line), "Already done: %d", skipped);
    draw_left(y, line);
    y += fa + 4;

    std::snprintf(line, sizeof(line), "Failed:       %d", failed_count_);
    draw_left(y, line);
    y += fa + 8;

    if (failed_count_ > 0) {
      buf.fill_rect(kLM, y, W - kLM * 2, 1, false);
      y += kPad;
      int shown = 0;
      for (const auto& job : jobs_) {
        if (!job.failed) continue;
        if (shown >= 5) { draw_left(y, "..."); break; }
        const std::string& t = job.title.empty() ? job.path : job.title;
        char entry[80];
        std::snprintf(entry, sizeof(entry), "%.70s", t.c_str());
        draw_left(y, entry);
        y += fa + 2;
        ++shown;
      }
    }
  }

  draw_centered(H - fa - kPad, "Back to return");
}

}  // namespace microreader
