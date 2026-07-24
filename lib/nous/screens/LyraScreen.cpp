#include "LyraScreen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/Book.h"
#include "../content/BookIndex.h"

namespace microreader {

static constexpr int kMaxCoverDisplayW = 100;  // thumbnail width for the recent-book card

// Cover mode blit: scale src to fill dst_w × dst_h, centered, crop overflow.
static void blit_cover(DrawBuffer& buf, int dst_x, int dst_y,
                        const uint8_t* data, int src_w, int src_h,
                        int dst_w, int dst_h) {
  if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
  const int src_stride = (src_w + 7) / 8;
  const int dst_stride = (dst_w + 7) / 8;
  uint8_t row_buf[80];
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
      const int bit = (src_row[sx >> 3] >> (7 - (sx & 7))) & 1;
      if (bit == 0)
        row_buf[dx >> 3] &= static_cast<uint8_t>(~(1u << (7 - (dx & 7))));
    }
    buf.blit_1bit_row(dst_x, dst_y + dy, row_buf, dst_w);
  }
}



void LyraScreen::on_start() {
  // Lyra is portrait-only; clamp landscape rotations to portrait and persist it.
  const Rotation rot = current_rotation_();
  if (rot == Rotation::Deg0 || rot == Rotation::Deg180) {
    set_buf_rotation_(Rotation::Deg90);
    if (app_) app_->set_rotate_display(0);  // 0 = Portrait (Deg90)
  }

  if (app_ && app_->data_dir_) {
    const std::string idx_path = std::string(app_->data_dir_) + "/book_index.dat";
    BookIndex::instance().load(idx_path);
  }

  has_recent_ = false;
  recent_title_.clear();
  recent_author_.clear();
  recent_path_.clear();

  const StringPool& pool = BookIndex::instance().pool();
  uint32_t best_order = 0;
  for (const auto& e : BookIndex::instance().entries()) {
    if (e.last_open_order > best_order) {
      best_order = e.last_open_order;
      recent_path_   = e.path.to_string(pool);
      recent_title_  = std::string(e.title.view(pool));
      recent_author_ = std::string(e.author.view(pool));
      has_recent_    = true;
    }
  }

  clear_items();
  int i = 0;
  if (has_recent_) {
    idx_recent_ = i++;
    add_item("");  // label unused; rendered custom in draw_all_
  } else {
    idx_recent_ = -1;
  }
  idx_all_books_    = i++; add_item("All Books");
  idx_recent_books_ = i++; add_item("Recent Books");
  idx_stats_        = i++; add_item("Stats");
  idx_settings_     = i++; add_item("Settings");

  // Cover image: compute path and try to load or flag for lazy extraction.
  cover_data_.clear();
  cover_loaded_        = false;
  cover_needs_extract_ = false;
  cover_bin_path_.clear();
  if (has_recent_ && app_ && app_->data_dir_) {
    cover_bin_path_ = cover_bin_path(recent_path_.c_str(), app_->data_dir_);
    FILE* chk = std::fopen(cover_bin_path_.c_str(), "rb");
    if (chk) {
      std::fclose(chk);
      load_cover_data_();
    } else {
      cover_needs_extract_ = true;
    }
  }
}

void LyraScreen::load_cover_data_() {
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

  // Scale to thumbnail display size during load — avoid keeping the full
  // high-res bitmap (up to 47 KB) in RAM when only ~100 px are displayed.
  const int dst_w = std::min(src_w, kMaxCoverDisplayW);
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
      if (std::fread(src_row.data(), 1, src_stride, f) != static_cast<size_t>(src_stride))
        { cover_data_.clear(); break; }
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

void LyraScreen::on_select(int index) {
  if (!app_) return;
  if (index == idx_recent_ && has_recent_) {
    app_->record_book_opened(recent_path_);
    app_->ensure_cover_bin(recent_path_);
    app_->reader()->set_path(recent_path_.c_str());
    app_->push_screen(ScreenId::Reader);
  } else if (index == idx_all_books_) {
    app_->push_screen(ScreenId::MainMenu);
  } else if (index == idx_recent_books_) {
    app_->push_screen(ScreenId::RecentBooks);
  } else if (index == idx_stats_) {
    app_->push_screen(ScreenId::GlobalStats);
  } else if (index == idx_settings_) {
    app_->push_screen(ScreenId::Settings);
  }
}

void LyraScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  // Lazy cover extraction: show loading bar, extract, then redraw.
  if (cover_needs_extract_) {
    cover_needs_extract_ = false;
    if (app_) app_->ensure_cover_bin(recent_path_);
    load_cover_data_();
    request_redraw();
    return;
  }

  // Long-press back (~3s) → Hidden Books; short press → no-op.
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

// Break text into lines fitting within max_w pixels using the given font style.
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
      int w = font.word_width(text.c_str() + line_start, word_end - line_start, style);
      if (w <= max_w || first_word) {
        line_end = word_end;
        i = word_end;
        first_word = false;
      } else {
        break;  // i stays at start of overflowing word
      }
    }
    lines.push_back(text.substr(line_start, line_end - line_start));
  }
  return lines;
}

void LyraScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  if (!ui_font_.valid()) return;
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  static constexpr int kPad        = 12;
  static constexpr int kTopGap     = 36;   // gap between header rule and card
  static constexpr int kCardPadV   = 20;
  static constexpr int kSubGap     = 4;
  static constexpr int kCoverGap   = 12;   // gap between cover and text column
  static constexpr int kCardNavGap = 24;   // extra gap between card and nav items
  static constexpr int kNavPadV    = 32;
  static constexpr int kBotPad     = 5;
  static constexpr int kBotMargin  = 10;

  const int ui_adv = ui_font_.y_advance();
  const int hf_adv = header_font_.valid() ? header_font_.y_advance() : ui_adv;
  const int sf_adv = section_font_.valid() ? section_font_.y_advance() : ui_adv;

  // ── Header ───────────────────────────────────────────────────────────────
  int y = 10;
  {
    const BitmapFont& brand_f = brand_font_.valid() ? brand_font_ : ui_font_;
    const BitmapFont& bf = section_font_.valid() ? section_font_ : ui_font_;
    // Centre both texts in hf_adv so they share the same visual baseline row.
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
  y += 1 + kTopGap;

  // ── Bottom tooltip height (pre-compute so nav items don't overflow into it)
  const int bot_area_h = 1 + kBotPad + sf_adv + kBotPad + kBotMargin;
  const int bot_rule_y = H - bot_area_h;

  // ── Recent book card ─────────────────────────────────────────────────────
  if (has_recent_ && idx_recent_ >= 0) {
    // Cover already loaded at kMaxCoverDisplayW; compute proportional display height.
    const int disp_cov_w = (cover_loaded_ && cover_w_ > 0)
        ? std::min(static_cast<int>(cover_w_), kMaxCoverDisplayW) : 0;
    const int disp_cov_h = (cover_loaded_ && cover_w_ > 0 && cover_h_ > 0)
        ? disp_cov_w * static_cast<int>(cover_h_) / static_cast<int>(cover_w_) : 0;

    // Determine text column bounds.
    const int cover_col_w = cover_loaded_ ? kPad + disp_cov_w + kCoverGap : 0;
    const int text_x      = kPad + cover_col_w;
    const int text_max_w  = W - text_x - kPad;

    // Wrap title into lines, bold if supported.
    const auto title_lines = wrap_text(recent_title_, ui_font_, text_max_w, FontStyle::Bold);
    const int n_title = static_cast<int>(title_lines.size());

    // Text group height: all title lines + gap + author.
    const bool has_author = section_font_.valid() && !recent_author_.empty();
    const int text_group_h = n_title * ui_adv + (has_author ? kSubGap + sf_adv : 0);

    // Card height: enough for text (with padding) or cover (with padding), whichever is taller.
    const int text_h = kCardPadV + text_group_h + kCardPadV;
    const int card_h = cover_loaded_
        ? std::max(text_h, disp_cov_h + kCardPadV * 2)
        : text_h;

    const bool sel = (selected() == idx_recent_);
    if (sel)
      buf.fill_rect(0, y, W, card_h, false);

    // Cover on the LEFT — scale high-res source down to thumbnail display size.
    if (cover_loaded_ && cover_w_ > 0 && cover_h_ > 0) {
      const int img_x = kPad;
      const int img_y = y + kCardPadV;
      blit_cover(buf, img_x, img_y,
                 cover_data_.data(), cover_w_, cover_h_,
                 disp_cov_w, disp_cov_h);
    }

    // Text group: vertically centered in card.
    int ty = y + (card_h - text_group_h) / 2;
    for (int li = 0; li < n_title; ++li) {
      buf.draw_text_proportional(text_x, ty + ui_font_.baseline(),
                                 title_lines[li].c_str(), title_lines[li].size(),
                                 ui_font_, sel, FontStyle::Bold);
      ty += ui_adv;
    }
    if (has_author) {
      ty += kSubGap;
      buf.draw_text_proportional(text_x, ty + section_font_.baseline(),
                                 recent_author_.c_str(), recent_author_.size(),
                                 section_font_, sel);
    }

    y += card_h + kCardNavGap;
    // No divider after card.
  }

  // ── Nav items (no dividers in Lyra theme) ────────────────────────────────
  const int nav_row_h = kNavPadV + ui_adv + kNavPadV;
  struct NavItem { int idx; const char* label; };
  const NavItem nav[] = {
    {idx_all_books_,    "All Books"},
    {idx_recent_books_, "Recent Books"},
    {idx_stats_,        "Stats"},
    {idx_settings_,     "Settings"},
  };
  for (const auto& item : nav) {
    if (item.idx < 0) continue;
    const bool sel = (selected() == item.idx);
    if (sel)
      buf.fill_rect(0, y, W, nav_row_h, false);
    buf.draw_text_proportional(kPad, y + kNavPadV + ui_font_.baseline(), item.label, ui_font_, sel);
    y += nav_row_h;
  }

  // ── Bottom button boxes ───────────────────────────────────────────────────
  if (section_font_.valid()) {
    const BitmapFont& sf = section_font_;
    const bool inv = app_ && app_->invert_menu_buttons();

    // Physical layout: 53px margin · 176px per pair · 22px gap · 176px · 53px
    static constexpr int kBoxLX = 53;
    static constexpr int kBoxW  = 176;
    static constexpr int kBoxRX = kBoxLX + kBoxW + 22;  // 251
    static constexpr int kLDiv  = kBoxLX + kBoxW / 2;   // 141
    static constexpr int kRDiv  = kBoxRX + kBoxW / 2;   // 339

    const int box_h  = kBotPad + sf.y_advance() + kBotPad;
    const int box_y  = bot_rule_y;
    const int text_y = box_y + kBotPad + sf.baseline();

    auto draw_box = [&](int bx) {
      buf.fill_rect(bx,            box_y,             kBoxW, 1,     false);
      buf.fill_rect(bx,            box_y + box_h - 1, kBoxW, 1,     false);
      buf.fill_rect(bx,            box_y,             1,     box_h, false);
      buf.fill_rect(bx + kBoxW - 1, box_y,            1,     box_h, false);
    };
    draw_box(kBoxLX);
    buf.fill_rect(kLDiv, box_y, 1, box_h, false);
    draw_box(kBoxRX);
    buf.fill_rect(kRDiv, box_y, 1, box_h, false);

    const char* labels[4] = {"Back", "Select", inv ? "Up" : "Down", inv ? "Down" : "Up"};
    const int centers[4]  = {kBoxLX + kBoxW / 4, kBoxLX + 3 * kBoxW / 4,
                              kBoxRX + kBoxW / 4, kBoxRX + 3 * kBoxW / 4};
    for (int i = 0; i < 4; ++i) {
      const int tw = static_cast<int>(sf.word_width(labels[i], std::strlen(labels[i]), FontStyle::Regular));
      buf.draw_text_proportional(centers[i] - tw / 2, text_y, labels[i], std::strlen(labels[i]), sf, false);
    }
  }
}

}  // namespace microreader
