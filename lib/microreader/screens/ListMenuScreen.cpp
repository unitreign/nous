#include "ListMenuScreen.h"

#include <cstring>

#include "../Application.h"
#include "../display/ui_font_header.h"
#include "../display/ui_font_medium.h"
#include "../display/ui_font_small.h"

namespace microreader {

int ListMenuScreen::font_size_idx_ = 0;

static constexpr int kHeaderY = 15;        // top padding before the title text
static constexpr int kBottomPadding = 16;  // list padding from bottom
static constexpr int kButtonHintsH = 26;   // height of the button hint area

void ListMenuScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  runtime_ = &runtime;
  // Re-init ui_font_ whenever the font_size_idx_ setting may have changed.
  ui_font_ = BitmapFont{};
  if (font_size_idx_ == 1)
    ui_font_.init(kFontData_ui_medium_mbf, kFontData_ui_medium_mbf_size);
  else
    ui_font_.init(kFontData_ui_small_mbf, kFontData_ui_small_mbf_size);
  if (!header_font_.valid())
    header_font_.init(kFontData_ui_header_mbf, kFontData_ui_header_mbf_size);
  const int prev_selected = selected_;
  clear_items();
  on_start_set_selection_ = false;
  on_start();
  // Restore selection if on_start() didn't explicitly call set_selected()
  // (e.g. returning from a sub-screen after navigating away).
  if (!on_start_set_selection_ && initial_selection_ >= 0 && initial_selection_ < count()) {
    selected_ = initial_selection_;
    initial_selection_ = -1;  // Only use once
  } else if (!on_start_set_selection_ && prev_selected > 0 && prev_selected < count()) {
    selected_ = prev_selected;
  }
  if (on_start_set_selection_ || selected_ > 0)
    center_on_selected_();
  else
    ensure_visible_();
  draw_all_(buf, runtime.battery_percentage());
}

void ListMenuScreen::ensure_visible_() {
  if (!ui_font_.valid() || count() == 0 || !buf_)
    return;
  const int line_h = ui_font_.y_advance() + 8;
  int subtitle_h = subtitle_.empty() ? 0 : ui_font_.y_advance() + 8;
  if (!subtitle2_.empty() && ui_font_.valid())
    subtitle_h += ui_font_.y_advance() + 8;
  if (!subtitle3_.empty() && ui_font_.valid())
    subtitle_h += ui_font_.y_advance() + 8;
  const int header_h = kHeaderY + (header_font_.valid() ? header_font_.y_advance() : 0) + subtitle_h + 8;
  const int visible = (buf_->height() - header_h - kBottomPadding) / line_h;
  if (visible <= 0)
    return;
  if (selected_ < scroll_offset_)
    scroll_offset_ = selected_;
  else if (selected_ >= scroll_offset_ + visible)
    scroll_offset_ = selected_ - visible + 1;

  // Scroll padding: keep at least 2 items visible above/below the selection.
  static constexpr int kPad = 2;
  if (selected_ - scroll_offset_ < kPad && scroll_offset_ > 0)
    scroll_offset_ = std::max(0, selected_ - kPad);
  else if (selected_ - scroll_offset_ > visible - 1 - kPad) {
    const int max_scroll = count() > visible ? count() - visible : 0;
    scroll_offset_ = std::min(max_scroll, selected_ - (visible - 1 - kPad));
  }
}

void ListMenuScreen::center_on_selected_() {
  if (!ui_font_.valid() || count() == 0 || !buf_)
    return;
  const int line_h = ui_font_.y_advance() + 8;
  int subtitle_h = subtitle_.empty() ? 0 : ui_font_.y_advance() + 8;
  if (!subtitle2_.empty() && ui_font_.valid())
    subtitle_h += ui_font_.y_advance() + 8;
  if (!subtitle3_.empty() && ui_font_.valid())
    subtitle_h += ui_font_.y_advance() + 8;
  const int header_h = kHeaderY + (header_font_.valid() ? header_font_.y_advance() : 0) + subtitle_h + 8;
  const int visible = (buf_->height() - header_h - kBottomPadding) / line_h;
  if (visible <= 0)
    return;
  // Center the selection: put it in the middle of the visible window.
  int offset = selected_ - visible / 2;
  const int max_scroll = count() > visible ? count() - visible : 0;
  if (offset < 0)
    offset = 0;
  if (offset > max_scroll)
    offset = max_scroll;
  scroll_offset_ = offset;
}

void ListMenuScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  const int W = buf.width();
  const int H = buf.height();
  const int n = count();

  buf.fill(true);

  // Title (header font)
  if (title_ && header_font_.valid()) {
    const size_t title_len = std::strlen(title_);
    const int tw = header_font_.word_width(title_, title_len, FontStyle::Regular);
    buf.draw_text_proportional((W - tw) / 2, kHeaderY + header_font_.baseline(), title_, header_font_, false);

    // Battery bar at the bottom center (horizontal)
    if (battery_pct.has_value()) {
      const int bat_pct = battery_pct.value();
      const int kBarW = 26;
      const int kBarH = 8;
      const int kBarX = (W - kBarW) / 2;  // Center horizontally
      const int kBarY = H - kBarH - 4;    // Position at the bottom

      // Outline: rounded corners
      buf.fill_rect(kBarX + 1, kBarY, kBarW - 2, 1, false);              // top edge
      buf.fill_rect(kBarX + 1, kBarY + kBarH - 1, kBarW - 2, 1, false);  // bottom edge
      buf.fill_rect(kBarX, kBarY + 1, 1, kBarH - 2, false);              // left edge
      buf.fill_rect(kBarX + kBarW - 1, kBarY + 1, 1, kBarH - 2, false);  // right edge

      // Inner bar: sloped right side (left to right)
      const int max_fill = kBarW - 4;
      const int filled = (bat_pct * max_fill) / 100;
      if (filled > 0) {
        buf.fill_row(kBarY + 5, kBarX + 2, kBarX + 2 + std::min(filled + 3, max_fill), false);
        buf.fill_row(kBarY + 4, kBarX + 2, kBarX + 2 + std::min(filled + 2, max_fill), false);
        buf.fill_row(kBarY + 3, kBarX + 2, kBarX + 2 + std::min(filled + 1, max_fill), false);
        buf.fill_row(kBarY + 2, kBarX + 2, kBarX + 2 + std::min(filled, max_fill), false);
      }
    }
  }

  int subtitle_h = 0;
  if (!subtitle_.empty() && ui_font_.valid()) {
    const size_t sub_len = subtitle_.size();
    const int sw = ui_font_.word_width(subtitle_.c_str(), sub_len, FontStyle::Regular);
    const int title_bottom = kHeaderY + (header_font_.valid() ? header_font_.y_advance() : 0);
    buf.draw_text_proportional((W - sw) / 2, title_bottom + ui_font_.baseline(), subtitle_.c_str(), sub_len, ui_font_,
                               false);
    subtitle_h += ui_font_.y_advance() + 8;
  }
  if (!subtitle2_.empty() && ui_font_.valid()) {
    const size_t sub2_len = subtitle2_.size();
    const int sw2 = ui_font_.word_width(subtitle2_.c_str(), sub2_len, FontStyle::Regular);
    const int title_bottom = kHeaderY + (header_font_.valid() ? header_font_.y_advance() : 0);
    buf.draw_text_proportional((W - sw2) / 2, title_bottom + subtitle_h + ui_font_.baseline() - 4, subtitle2_.c_str(),
                               sub2_len, ui_font_, false);
    subtitle_h += ui_font_.y_advance() + 8;
  }
  if (!subtitle3_.empty() && ui_font_.valid()) {
    const size_t sub3_len = subtitle3_.size();
    const int sw3 = ui_font_.word_width(subtitle3_.c_str(), sub3_len, FontStyle::Regular);
    const int title_bottom = kHeaderY + (header_font_.valid() ? header_font_.y_advance() : 0);
    buf.draw_text_proportional((W - sw3) / 2, title_bottom + subtitle_h + ui_font_.baseline() - 4, subtitle3_.c_str(),
                               sub3_len, ui_font_, false);
    subtitle_h += ui_font_.y_advance() + 8;
  }

  if (!ui_font_.valid() || n == 0) {
    if (n == 0 && ui_font_.valid()) {
      static const char* kEmpty = "No items";
      const int ew = ui_font_.word_width(kEmpty, std::strlen(kEmpty), FontStyle::Regular);
      buf.draw_text_proportional((W - ew) / 2, H / 2 + ui_font_.baseline(), kEmpty, ui_font_, false);
    }
    return;
  }

  const int line_h = ui_font_.y_advance() + 8;
  const int baseline = ui_font_.baseline();
  // Provide padding below the header
  const int header_h = kHeaderY + (header_font_.valid() ? header_font_.y_advance() : 0) + subtitle_h + 8;
  const int visible = (H - header_h - kBottomPadding) / line_h;

  const int end = scroll_offset_ + visible < n ? scroll_offset_ + visible : n;

  // Compute total height for centring when all items fit on screen.
  // Separators are drawn as a thin line taking half a line slot.
  int total_h = 0;
  for (int i = scroll_offset_; i < end; ++i)
    total_h += (i < (int)separators_.size() && separators_[i]) ? line_h / 2 : line_h;

  const int items_y = n <= visible ? header_h + (H - kBottomPadding - header_h - total_h) / 2 : header_h;

  static const char kEllipsis[] = "...";
  const int ellipsis_w = ui_font_.word_width(kEllipsis, 3, FontStyle::Regular);
  char trunc_buf[260];

  int y = items_y;
  for (int i = scroll_offset_; i < end; ++i) {
    const bool is_sep = (i < (int)separators_.size() && separators_[i]);
    if (is_sep) {
      y += line_h / 2;
      continue;
    }
    const std::string& label_str = labels_[i];
    const char* label = label_str.c_str();
    size_t len = label_str.size();
    int iw = ui_font_.word_width(label, len, FontStyle::Regular);

    const int landscape_pad = (buf.rotation() == Rotation::Deg0) ? 10 : 0;
    const int indent_px = align_left_ ? (32 + ((i < (int)indents_.size() ? indents_[i] : 0) * 20)) : 0;
    const int max_item_w = align_left_ ? (W - 32 - landscape_pad - indent_px) : (W - 120);

    // Truncate with "..." if the label is too wide to fit.
    if (iw > max_item_w) {
      const int budget = max_item_w - ellipsis_w;
      size_t fit = 0;
      const char* p = label;
      while (*p) {
        const uint8_t b = static_cast<uint8_t>(*p);
        const size_t cb = b < 0x80 ? 1u : b < 0xE0 ? 2u : b < 0xF0 ? 3u : 4u;
        if (ui_font_.word_width(label, fit + cb, FontStyle::Regular) > budget)
          break;
        fit += cb;
        p += cb;
      }
      const size_t copy = fit < 256 ? fit : 256;
      std::memcpy(trunc_buf, label, copy);
      std::memcpy(trunc_buf + copy, kEllipsis, 3);
      trunc_buf[copy + 3] = '\0';
      label = trunc_buf;
      len = copy + 3;
      iw = ui_font_.word_width(label, len, FontStyle::Regular);
    }

    const int ix = align_left_ ? indent_px : (W - iw) / 2;
    const int iy = y;
    if (i == selected_) {
      const int bar_w = 3;  // horizontal padding on each side (rounded cap adds 1 more)
      const int sel_top = (font_size_idx_ == 1) ? iy : iy - 1;
      const int bar_h = ui_font_.y_advance() + (font_size_idx_ == 1 ? 1 : 2) - 1;

      if (align_left_) {
        // Full width bar for left-aligned
        const int bar_x = 16;
        const int bar_width = W - 32 - landscape_pad;
        buf.fill_rect(bar_x + 1, sel_top, bar_width - 2, bar_h, false);          // Body
        buf.fill_rect(bar_x, sel_top + 1, 1, bar_h - 2, false);                  // Left cap
        buf.fill_rect(bar_x + bar_width - 1, sel_top + 1, 1, bar_h - 2, false);  // Right cap
        buf.draw_text_proportional(ix, iy + baseline, label, len, ui_font_, true);
      } else {
        buf.fill_rect(ix - bar_w, sel_top, iw + bar_w * 2, bar_h, false);
        buf.fill_rect(ix - bar_w - 1, sel_top + 1, 1, bar_h - 2, false);
        buf.fill_rect(ix + iw + bar_w, sel_top + 1, 1, bar_h - 2, false);
        buf.draw_text_proportional(ix, iy + baseline, label, len, ui_font_, true);
      }
    } else {
      buf.draw_text_proportional(ix, iy + baseline, label, len, ui_font_, false);
    }
    y += line_h;
  }

  // Scrollbar when items overflow. Left edge in landscape, right edge in portrait.
  if (n > visible) {
    const int sb_w = 4;
    const int sb_x = (buf.rotation() == Rotation::Deg0) ? 8 : W - 12;
    const int sb_top = items_y - (font_size_idx_ == 1 ? 0 : 1);
    const int sb_bottom = items_y + total_h - line_h + ui_font_.y_advance();
    const int sb_total_h = sb_bottom - sb_top;
    const int thumb_min = 20;
    const int thumb_h = std::max(sb_total_h * visible / n, thumb_min);
    const int track = sb_total_h - thumb_h;
    const int max_scroll = n - visible;
    const int thumb_y = sb_top + (max_scroll > 0 ? track * scroll_offset_ / max_scroll : 0);

    // Draw rounded scrollbar thumb (width 4)
    buf.fill_rect(sb_x + 1, thumb_y, sb_w - 2, 1, false);                // Top cap
    buf.fill_rect(sb_x, thumb_y + 1, sb_w, thumb_h - 2, false);          // Body
    buf.fill_rect(sb_x + 1, thumb_y + thumb_h - 1, sb_w - 2, 1, false);  // Bottom cap
  }

  draw_button_hints_(buf);
}

void ListMenuScreen::draw_button_hints_(DrawBuffer& buf) const {
  if (!ui_font_.valid())
    return;
  const int W = buf.width();
  const int H = buf.height();
  const int baseline = ui_font_.baseline();

  // Four labels: back=◀, select=▶, down=▼, up=▲
  bool inv_menu = app_ && app_->invert_menu_buttons();
  const char* lbl_down = "\xe2\x96\xbc";
  const char* lbl_up = "\xe2\x96\xb2";
  const char* kLabels[4] = {"\xe2\x97\x80", "\xe2\x96\xb6", inv_menu ? lbl_up : lbl_down, inv_menu ? lbl_down : lbl_up};
  static const size_t kLens[4] = {3, 3, 3, 3};

  const bool sideways = buf.rotation() == Rotation::Deg90;
  const int L = sideways ? W : H;
  const int pair0 = L * 163 / 550;
  const int pair1 = L - pair0;
  const int gap = 50;
  const int btns[4] = {pair0 - gap, pair0 + gap, pair1 - gap, pair1 + gap};

  for (int i = 0; i < 4; ++i) {
    const int lw = ui_font_.word_width(kLabels[i], kLens[i], FontStyle::Regular);
    if (!sideways) {
      const int text_x = W - kButtonHintsH / 2 - lw / 2;
      // In landscape, if derived from portrait by rotating -90 deg,
      // the original left button (X=0) becomes the bottom right button (Y=MAX).
      // So we map i -> 3 - i if we need to reverse the order on the Y axis
      // because pair0 is smaller and pair1 is larger.
      int mapped_i = 3 - i;
      const int text_y = btns[mapped_i] - ui_font_.y_advance() / 2 + baseline;
      buf.draw_text_proportional(text_x, text_y, kLabels[i], kLens[i], ui_font_, false);
    } else {
      const int text_y = H - kButtonHintsH / 2 - ui_font_.y_advance() / 2 + baseline + 5;
      buf.draw_text_proportional(btns[i] - lw / 2, text_y, kLabels[i], kLens[i], ui_font_, false);
    }
  }
}

void ListMenuScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  const int n = count();

  // Helper lambdas for selection movement (skip separators).
  auto move_up = [&]() {
    int next = selected_ > 0 ? selected_ - 1 : n - 1;
    while (next != selected_ && next < (int)separators_.size() && separators_[next])
      next = next > 0 ? next - 1 : n - 1;
    selected_ = next;
    ensure_visible_();
  };
  auto move_down = [&]() {
    int next = selected_ < n - 1 ? selected_ + 1 : 0;
    while (next != selected_ && next < (int)separators_.size() && separators_[next])
      next = next < n - 1 ? next + 1 : 0;
    selected_ = next;
    ensure_visible_();
  };

  bool moved = false;       // selection changed — needs a redraw
  bool needs_draw = false;  // on_select returned true — needs a redraw

  // Track whether a fresh press event arrived this frame for each nav direction.
  bool had_up_press = false;
  bool had_down_press = false;

  bool inv_menu = app_ && app_->invert_menu_buttons();
  Button logical_up = inv_menu ? Button::Button2 : Button::Button3;
  Button logical_down = inv_menu ? Button::Button3 : Button::Button2;

  Button btn;
  while (buttons.next_press(btn)) {
    if (btn == logical_up) {
      if (n > 0) {
        move_up();
        moved = true;
        had_up_press = true;
      }
    } else if (btn == logical_down) {
      if (n > 0) {
        move_down();
        moved = true;
        had_down_press = true;
      }
    } else {
      switch (btn) {
        case Button::Button0:
          // Flush any pending move before back so the screen redraws correctly
          // if on_back() decides to stay.
          if (moved) {
            draw_all_(buf, runtime.battery_percentage());
            buf.refresh();
            moved = false;
          }
          on_back();
          if (app_ && app_->has_pending_transition()) {
            return;
          }
          break;

        case Button::Button1:  // select
          if (n > 0 && selected_ < n) {
            on_select(selected_);
            if (app_ && app_->has_pending_transition()) {
              return;
            }
            needs_draw = true;
          }
          break;

        case Button::Up:  // physical up
          if (n > 0) {
            move_up();
            moved = true;
            had_up_press = true;
          }
          break;

        case Button::Down:  // physical down
          if (n > 0) {
            move_down();
            moved = true;
            had_down_press = true;
          }
          break;

        default:
          break;
      }
    }
  }

  // Hold-down acceleration: when a nav button is held (no fresh press this frame),
  // step size grows by 1 each frame: frame 0 = 1, frame 1 = 2, frame 2 = 3, …
  auto hold_step = [](int frames) -> int { return frames + 1; };

  const bool up_held = !had_up_press && (buttons.is_down(logical_up) || buttons.is_down(Button::Up));
  const bool down_held = !had_down_press && (buttons.is_down(logical_down) || buttons.is_down(Button::Down));

  if (up_held && n > 0) {
    const int step = hold_step(hold_frames_up_);
    for (int i = 0; i < step; ++i)
      move_up();
    ++hold_frames_up_;
    moved = true;
  } else if (!had_up_press) {
    hold_frames_up_ = 0;
  }

  if (down_held && n > 0) {
    const int step = hold_step(hold_frames_down_);
    for (int i = 0; i < step; ++i)
      move_down();
    ++hold_frames_down_;
    moved = true;
  } else if (!had_down_press) {
    hold_frames_down_ = 0;
  }

  if (moved || needs_draw) {
    draw_all_(buf, runtime.battery_percentage());
    buf.refresh();
  }
}

void ListMenuScreen::on_back() {
  if (app_)
    app_->pop_screen();
}

}  // namespace microreader
