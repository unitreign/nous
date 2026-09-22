#include "BentoScreen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/Book.h"
#include "../content/BookIndex.h"

namespace microreader {

// Scale src to fill dst_w x dst_h, centered, crop overflow.
static void blit_cover(DrawBuffer& buf, int dst_x, int dst_y,
                       const uint8_t* data, int src_w, int src_h,
                       int dst_w, int dst_h) {
  if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
  const int src_stride = (src_w + 7) / 8;
  const int dst_stride = (dst_w + 7) / 8;
  uint8_t row_buf[64];
  if (dst_stride > (int)sizeof(row_buf)) return;
  const bool wd = (dst_w * src_h >= dst_h * src_w);
  const int crop_y = wd ? ((src_h * dst_w / src_w) - dst_h) / 2 : 0;
  const int crop_x = wd ? 0 : ((src_w * dst_h / src_h) - dst_w) / 2;
  for (int dy = 0; dy < dst_h; ++dy) {
    const int sy = wd
        ? std::min((dy + crop_y) * src_w / dst_w, src_h - 1)
        : std::min(dy * src_h / dst_h, src_h - 1);
    const uint8_t* src_row = data + sy * src_stride;
    std::memset(row_buf, 0xFF, static_cast<size_t>(dst_stride));
    for (int dx = 0; dx < dst_w; ++dx) {
      const int sx = wd
          ? std::min(dx * src_w / dst_w, src_w - 1)
          : std::min((dx + crop_x) * src_h / dst_h, src_w - 1);
      if (!((src_row[sx >> 3] >> (7 - (sx & 7))) & 1))
        row_buf[dx >> 3] &= static_cast<uint8_t>(~(1u << (7 - (dx & 7))));
    }
    buf.blit_1bit_row(dst_x, dst_y + dy, row_buf, dst_w);
  }
}

static std::vector<std::string> wrap_text(const std::string& text, const BitmapFont& font,
                                          int max_w, FontStyle style) {
  std::vector<std::string> lines;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    const size_t line_start = i;
    size_t line_end = i;
    bool first_word = true;
    while (i < n) {
      while (i < n && text[i] == ' ') ++i;
      if (i >= n) break;
      size_t word_end = i;
      while (word_end < n && text[word_end] != ' ') ++word_end;
      if (font.word_width(text.c_str() + line_start, word_end - line_start, style) <= max_w || first_word) {
        line_end = word_end;
        i = word_end;
        first_word = false;
      } else {
        break;
      }
    }
    lines.push_back(text.substr(line_start, line_end - line_start));
  }
  return lines;
}

void BentoScreen::on_start() {
  home_screen_selector_ = true;
  const Rotation rot = current_rotation_();
  if (rot == Rotation::Deg0 || rot == Rotation::Deg180) {
    set_buf_rotation_(Rotation::Deg90);
    if (app_) app_->set_rotate_display(0);
  }

  if (app_ && app_->data_dir_) {
    const std::string idx_path = std::string(app_->data_dir_) + "/book_index.dat";
    BookIndex::instance().load(idx_path);
  }

  has_recent_ = false;
  recent_title_.clear();
  recent_author_.clear();
  recent_path_.clear();
  recent_progress_pct_ = 0;

  const StringPool& pool = BookIndex::instance().pool();
  uint32_t best_order = 0;
  for (const auto& e : BookIndex::instance().entries()) {
    if (e.last_open_order > best_order) {
      best_order = e.last_open_order;
      recent_path_         = e.path.to_string(pool);
      recent_title_        = std::string(e.title.view(pool));
      recent_author_       = std::string(e.author.view(pool));
      recent_progress_pct_ = e.progress_pct;
      has_recent_          = true;
    }
  }

  clear_items();
  int i = 0;
  if (has_recent_) {
    idx_recent_ = i++;
    add_item("");
  } else {
    idx_recent_ = -1;
  }
  idx_all_books_    = i++; add_item("All Books");
  idx_recent_books_ = i++; add_item("Recent Books");
  idx_series_       = i++; add_item("Series");
  idx_stats_        = i++; add_item("Stats");
  idx_settings_     = i++; add_item("Settings");

  cover_data_.clear();
  cover_loaded_        = false;
  cover_needs_extract_ = false;
  cover_bin_path_.clear();

  if (has_recent_ && app_ && app_->data_dir_) {
    const std::string sleep_path = cover_sleep_bin_path(recent_path_.c_str(), app_->data_dir_);
    FILE* sf = std::fopen(sleep_path.c_str(), "rb");
    if (sf) { std::fclose(sf); cover_bin_path_ = sleep_path; }
    else cover_bin_path_ = cover_bin_path(recent_path_.c_str(), app_->data_dir_);

    FILE* chk = std::fopen(cover_bin_path_.c_str(), "rb");
    if (chk) {
      std::fclose(chk);
      load_cover_data_();
      if (!sf && app_->sleep_is_book_cover())
        cover_needs_extract_ = true;
    } else {
      cover_needs_extract_ = true;
    }
  }
}

void BentoScreen::load_cover_data_() {
  cover_data_.clear();
  cover_loaded_ = false;
  cover_w_ = cover_h_ = 0;
  if (cover_bin_path_.empty()) return;
  FILE* f = std::fopen(cover_bin_path_.c_str(), "rb");
  if (!f) return;
  uint16_t hdr[2] = {};
  if (std::fread(hdr, 2, 2, f) != 2) { std::fclose(f); return; }
  const int src_w = hdr[0], src_h = hdr[1];
  if (src_w <= 0 || src_h <= 0) { std::fclose(f); return; }

  const int dst_w = std::min(src_w, kCoverTargetW);
  const int dst_h = dst_w * src_h / src_w;
  if (dst_w <= 0 || dst_h <= 0) { std::fclose(f); return; }

  const int src_stride = (src_w + 7) / 8;
  const int dst_stride = (dst_w + 7) / 8;
  cover_data_.assign(static_cast<size_t>(dst_stride) * dst_h, 0xFF);
  std::vector<uint8_t> src_row(src_stride);
  int prev_sy = -1;
  for (int dy = 0; dy < dst_h; ++dy) {
    const int sy = dy * src_h / dst_h;
    if (sy != prev_sy) {
      std::fseek(f, 4 + static_cast<long>(sy) * src_stride, SEEK_SET);
      if (std::fread(src_row.data(), 1, src_stride, f) != static_cast<size_t>(src_stride)) {
        cover_data_.clear();
        break;
      }
      prev_sy = sy;
    }
    uint8_t* dr = cover_data_.data() + static_cast<size_t>(dy) * dst_stride;
    for (int dx = 0; dx < dst_w; ++dx) {
      const int sx = dx * src_w / dst_w;
      if (!((src_row[sx >> 3] >> (7 - (sx & 7))) & 1))
        dr[dx >> 3] &= static_cast<uint8_t>(~(1u << (7 - (dx & 7))));
    }
  }
  if (!cover_data_.empty()) {
    cover_w_ = static_cast<uint16_t>(dst_w);
    cover_h_ = static_cast<uint16_t>(dst_h);
    cover_loaded_ = true;
  }
  std::fclose(f);
}

void BentoScreen::on_select(int index) {
  if (!app_) return;
  if (index == idx_recent_ && has_recent_) {
    show_opening_indicator();
    app_->record_book_opened(recent_path_);
    app_->reader()->set_path(recent_path_.c_str());
    app_->push_screen(ScreenId::Reader);
  } else if (index == idx_all_books_) {
    app_->push_screen(ScreenId::MainMenu);
  } else if (index == idx_recent_books_) {
    app_->push_screen(ScreenId::RecentBooks);
  } else if (index == idx_series_) {
    app_->push_screen(ScreenId::SeriesList);
  } else if (index == idx_stats_) {
    app_->push_screen(ScreenId::GlobalStats);
  } else if (index == idx_settings_) {
    app_->push_screen(ScreenId::Settings);
  }
}

void BentoScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  if (cover_needs_extract_) {
    cover_needs_extract_ = false;
    if (app_) app_->ensure_cover_bin(recent_path_, buf.scratch_buf1(), buf.scratch_buf2(), DrawBuffer::kBufSize, true);
    load_cover_data_();
    request_redraw();
    return;
  }

  const bool back_down = buttons.is_down(Button::Button0);
  ButtonState fwd = buttons;
  if (back_down) {
    if (back_hold_frames_ <= kHiddenHoldFrames)
      back_hold_frames_++;
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
    if (held >= kHiddenHoldFrames && app_)
      app_->push_screen(ScreenId::HiddenBooks);
    return;
  }
  ListMenuScreen::update(fwd, buf, runtime);
}

void BentoScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  if (!ui_font_.valid()) return;
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  static constexpr int kPad      = 12;
  static constexpr int kBotPad   = 5;
  static constexpr int kBotMargin = 10;

  const int ui_adv = ui_font_.y_advance();
  const int hf_adv = header_font_.valid() ? header_font_.y_advance() : ui_adv;
  const int sf_adv = section_font_.valid() ? section_font_.y_advance() : ui_adv;

  // ── Header ───────────────────────────────────────────────────────────────
  int y = 10;
  {
    const BitmapFont& brand_f = brand_font_.valid() ? brand_font_ : ui_font_;
    const BitmapFont& bf = section_font_.valid() ? section_font_ : ui_font_;
    const int nous_y = y + (hf_adv - brand_f.y_advance()) / 2 + brand_f.baseline();
    buf.draw_text_proportional(kPad, nous_y, "nous", 4, brand_f, false);

    if (battery_pct) {
      char pbuf[8];
      std::snprintf(pbuf, sizeof(pbuf), "%u%%", static_cast<unsigned>(*battery_pct));
      const int pw = bf.word_width(pbuf, std::strlen(pbuf), FontStyle::Regular);
      const int bat_y = y + (hf_adv - bf.y_advance()) / 2 + bf.baseline();
      buf.draw_text_proportional(W - kPad - pw, bat_y, pbuf, bf, false);
    }
  }
  y += hf_adv + 8;
  buf.fill_rect(0, y, W, 1, false);
  y += 1;

  const int content_top = y;

  // ── Tooltip bar pre-compute ──────────────────────────────────────────────
  const int bot_area_h = 1 + kBotPad + sf_adv + kBotPad + kBotMargin;
  const int bot_rule_y = H - bot_area_h;
  const int content_h  = bot_rule_y - content_top;

  // ── Grid dimensions ──────────────────────────────────────────────────────
  const int bottom_row_h = content_h * 27 / 100;
  const int bottom_row_y = bot_rule_y - bottom_row_h;

  // Main section (above bottom row, minus 1px divider)
  const int main_y = content_top;
  const int main_h = bottom_row_y - main_y - 1;

  // Left panel | right panels
  const int left_w   = W * 62 / 100;
  const int right_x  = left_w + 1;
  const int right_w  = W - right_x;

  // Right section split horizontally
  const int right_top_h = main_h / 2;
  const int right_mid_y = main_y + right_top_h;
  const int right_bot_y = right_mid_y + 1;
  const int right_bot_h = main_h - right_top_h - 1;

  // Bottom row: 3 equal columns
  const int col_w = W / 3;

  // ── Dividers ──────────────────────────────────────────────────────────────
  buf.fill_rect(0,         bottom_row_y - 1, W,       1,             false);
  buf.fill_rect(left_w,    main_y,           1,        main_h,        false);
  buf.fill_rect(right_x,   right_mid_y,      right_w,  1,             false);
  buf.fill_rect(col_w,     bottom_row_y,     1,        bottom_row_h,  false);
  buf.fill_rect(col_w * 2, bottom_row_y,     1,        bottom_row_h,  false);

  const bool is_cover_mode = (theme_ == MenuTheme::BentoCover);

  // ── Left panel (recent book) ─────────────────────────────────────────────
  {
    const bool sel = (idx_recent_ >= 0 && selected() == idx_recent_);
    const bool inv = sel && sel_fills_bg_();

    if (is_cover_mode && cover_loaded_ && cover_w_ > 0 && cover_h_ > 0) {
      blit_cover(buf, 0, main_y,
                 cover_data_.data(), cover_w_, cover_h_,
                 left_w, main_h);
      if (sel) {
        if (inv) {
          // Block: invert-border indicator over the cover.
          static constexpr int kBorderPx = 3;
          buf.fill_rect(0,               main_y,               left_w,    kBorderPx, false);
          buf.fill_rect(0,               main_y + main_h - kBorderPx, left_w, kBorderPx, false);
          buf.fill_rect(0,               main_y,               kBorderPx, main_h,    false);
          buf.fill_rect(left_w - kBorderPx, main_y,           kBorderPx, main_h,    false);
        } else {
          draw_item_sel_(buf, 0, main_y, left_w, main_h);
        }
      }
    } else {
      if (sel) {
        if (inv) buf.fill_rect(0, main_y, left_w, main_h, false);
        else     draw_item_sel_(buf, 0, main_y, left_w, main_h);
      }

      if (has_recent_) {
        const int inner_x = kPad;
        const int inner_w = left_w - 2 * kPad;
        const BitmapFont& title_f  = header_font_.valid() ? header_font_ : ui_font_;
        const BitmapFont& author_f = section_font_.valid() ? section_font_ : ui_font_;

        const std::string display_title = (recent_progress_pct_ >= 100)
            ? recent_title_ + " (fin)" : recent_title_;
        const auto title_lines = wrap_text(display_title, title_f, inner_w, FontStyle::Regular);

        const int title_adv  = title_f.y_advance();
        const int author_adv = author_f.y_advance();
        const bool has_author = !recent_author_.empty();
        const int group_h = (int)title_lines.size() * title_adv + (has_author ? 8 + author_adv : 0);

        int ty = main_y + (main_h - group_h) / 2;
        for (const auto& line : title_lines) {
          const int tw = title_f.word_width(line.c_str(), line.size(), FontStyle::Regular);
          const int tx = inner_x + std::max(0, (inner_w - tw) / 2);
          buf.draw_text_proportional(tx, ty + title_f.baseline(), line.c_str(), line.size(),
                                     title_f, inv);
          ty += title_adv;
        }
        if (has_author) {
          ty += 8;
          const int aw = author_f.word_width(recent_author_.c_str(), recent_author_.size(), FontStyle::Regular);
          const int ax = inner_x + std::max(0, (inner_w - aw) / 2);
          buf.draw_text_proportional(ax, ty + author_f.baseline(), recent_author_.c_str(),
                                     recent_author_.size(), author_f, inv);
        }
      }
    }
  }

  // ── Cell helper ──────────────────────────────────────────────────────────
  auto draw_cell = [&](int cx, int cy, int cw, int ch, const char* label, bool sel_c) {
    if (sel_c) draw_item_sel_(buf, cx, cy, cw, ch);
    const bool inv_c = sel_c && sel_fills_bg_();
    const int lw = ui_font_.word_width(label, std::strlen(label), FontStyle::Regular);
    const int lx = cx + std::max(0, (cw - lw) / 2);
    const int ly = cy + (ch - ui_adv) / 2 + ui_font_.baseline();
    buf.draw_text_proportional(lx, ly, label, ui_font_, inv_c);
  };

  // ── Right panels ─────────────────────────────────────────────────────────
  draw_cell(right_x, main_y,      right_w, right_top_h, "All Books",    selected() == idx_all_books_);
  draw_cell(right_x, right_bot_y, right_w, right_bot_h, "Recent Books", selected() == idx_recent_books_);

  // ── Bottom row ────────────────────────────────────────────────────────────
  draw_cell(0,          bottom_row_y, col_w,           bottom_row_h, "Series",   selected() == idx_series_);
  draw_cell(col_w,      bottom_row_y, col_w,           bottom_row_h, "Stats",    selected() == idx_stats_);
  draw_cell(col_w * 2,  bottom_row_y, W - col_w * 2,  bottom_row_h, "Settings", selected() == idx_settings_);

  // ── Tooltip bar ───────────────────────────────────────────────────────────
  if (section_font_.valid()) {
    const char* labels[4];
    get_button_labels(labels);
    draw_lyra_tooltip_bar(buf, section_font_, W, bot_rule_y, labels);
  }
}

}  // namespace microreader
