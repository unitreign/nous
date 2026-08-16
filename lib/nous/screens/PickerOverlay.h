#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Input.h"
#include "../content/BitmapFont.h"
#include "../display/DrawBuffer.h"

namespace microreader {

// Reusable popup picker overlay. Draws a centered list over the current screen.
// Call open() to show, update() each frame while is_open(), draw() after updating.
// on_select is called with the chosen index; on_cancel is called on back.
class PickerOverlay {
 public:
  void init(const BitmapFont& font) { font_ = &font; }

  void open(const char* title, std::vector<std::string> options, int initial_sel,
            std::function<void(int)> on_select,
            std::function<void()> on_cancel = nullptr) {
    title_     = title;
    options_   = std::move(options);
    sel_       = initial_sel;
    scroll_    = std::max(0, initial_sel - (kMaxVisible - 1));
    on_select_ = std::move(on_select);
    on_cancel_ = std::move(on_cancel);
    open_      = true;
  }

  bool is_open() const { return open_; }

  // Returns true if input was consumed and a redraw is needed.
  bool update(const ButtonState& buttons, bool invert_up_down = false) {
    if (!open_) return false;
    const int n = static_cast<int>(options_.size());
    bool redraw = false;
    Button btn;
    while (buttons.next_press(btn)) {
      const bool up   = invert_up_down ? (btn == Button::Button3 || btn == Button::Down)
                                       : (btn == Button::Button2 || btn == Button::Up);
      const bool down = invert_up_down ? (btn == Button::Button2 || btn == Button::Up)
                                       : (btn == Button::Button3 || btn == Button::Down);
      if (up) {
        sel_ = (sel_ - 1 + n) % n;
        if (sel_ < scroll_) scroll_ = sel_;
        else if (sel_ == n - 1) scroll_ = std::max(0, n - kMaxVisible);
        redraw = true;
      } else if (down) {
        sel_ = (sel_ + 1) % n;
        if (sel_ == 0) scroll_ = 0;
        else if (sel_ >= scroll_ + kMaxVisible) scroll_ = sel_ - kMaxVisible + 1;
        redraw = true;
      } else if (btn == Button::Button1) {
        open_ = false;
        if (on_select_) on_select_(sel_);
        return true;
      } else if (btn == Button::Button0) {
        open_ = false;
        if (on_cancel_) on_cancel_();
        return true;
      }
    }
    return redraw;
  }

  void draw(DrawBuffer& buf) const {
    if (!open_ || !font_ || !font_->valid()) return;
    const int W = buf.width();
    const int H = buf.height();
    const int n = static_cast<int>(options_.size());

    static constexpr int kPadH    = 20;
    static constexpr int kPadV    = 40;
    static constexpr int kRowPad  = 8;
    static constexpr int kTitlePad= 8;
    static constexpr int kScrollW = 6;

    const int row_h   = kRowPad + font_->y_advance() + kRowPad;
    const int title_h = kTitlePad + font_->y_advance() + kTitlePad;
    const int max_vis = std::max(1, (H - 2 * kPadV - 1 - title_h - 1 - 1) / row_h);

    // Clamp scroll
    int scroll = scroll_;
    if (sel_ < scroll) scroll = sel_;
    if (sel_ >= scroll + max_vis) scroll = sel_ - max_vis + 1;
    if (scroll > n - max_vis) scroll = std::max(0, n - max_vis);

    const int vis     = std::min(max_vis, n - scroll);
    const bool need_sb= (n > max_vis);
    const int popup_h = 1 + title_h + 1 + vis * row_h + 1;
    const int popup_x = kPadH;
    const int popup_w = W - 2 * kPadH;
    const int popup_y = std::max(0, (H - popup_h) / 2);
    const int text_x  = popup_x + 1 + 10;

    buf.fill_rect(popup_x, popup_y, popup_w, popup_h, true);
    buf.fill_rect(popup_x, popup_y, popup_w, 1, false);
    buf.fill_rect(popup_x, popup_y + popup_h - 1, popup_w, 1, false);
    buf.fill_rect(popup_x, popup_y, 1, popup_h, false);
    buf.fill_rect(popup_x + popup_w - 1, popup_y, 1, popup_h, false);

    int py = popup_y + 1;
    buf.draw_text_proportional(text_x, py + kTitlePad + font_->baseline(),
                               title_.c_str(), title_.size(), *font_, false);
    py += title_h;
    buf.fill_rect(popup_x + 1, py, popup_w - 2, 1, false);
    py += 1;

    for (int i = 0; i < vis; ++i) {
      const int idx = scroll + i;
      const bool sel = (idx == sel_);
      if (sel) buf.fill_rect(popup_x + 1, py, popup_w - 2, row_h, false);
      buf.draw_text_proportional(text_x, py + kRowPad + font_->baseline(),
                                 options_[idx].c_str(), options_[idx].size(), *font_, sel);
      py += row_h;
    }

    if (need_sb) {
      const int tx = popup_x + popup_w - 1 - kScrollW;
      const int ty = popup_y + 1 + title_h + 1;
      const int th = vis * row_h;
      const int tb = std::max(4, th * max_vis / n);
      const int to = ty + (th - tb) * scroll / std::max(1, n - max_vis);
      buf.fill_rect(tx, ty, kScrollW, th, true);
      buf.fill_rect(tx, to, kScrollW, tb, false);
    }
  }

 private:
  static constexpr int kMaxVisible = 6;

  const BitmapFont* font_ = nullptr;
  std::string title_;
  std::vector<std::string> options_;
  int sel_    = 0;
  int scroll_ = 0;
  bool open_  = false;
  std::function<void(int)> on_select_;
  std::function<void()>    on_cancel_;
};

}  // namespace microreader
