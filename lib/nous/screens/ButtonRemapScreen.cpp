#include "ButtonRemapScreen.h"

#include <cstring>

#include "../Application.h"
#include "../display/ui_font_large.h"
#include "../display/ui_font_medium.h"
#include "../display/ui_font_small.h"

namespace microreader {

void ButtonRemapScreen::load_fonts_() {
  if (!title_font_.valid())
    title_font_.init(kFontData_ui_large_mbf, kFontData_ui_large_mbf_size);
  if (!body_font_.valid())
    body_font_.init(kFontData_ui_medium_mbf, kFontData_ui_medium_mbf_size);
  if (!hint_font_.valid())
    hint_font_.init(kFontData_ui_small_mbf, kFontData_ui_small_mbf_size);
}

static int text_w(const BitmapFont& f, const char* s) {
  return static_cast<int>(f.word_width(s, std::strlen(s), FontStyle::Regular));
}

static void draw_text(DrawBuffer& buf, int x, int baseline_y, const char* text,
                      const BitmapFont& font, bool white) {
  buf.draw_text_proportional(x, baseline_y, text, font, white, FontStyle::Regular);
}

void ButtonRemapScreen::draw_(DrawBuffer& buf) {
  load_fonts_();
  const int W = buf.width();
  const int H = buf.height();

  buf.fill_rect(0, 0, W, H, true);  // white background

  int y = 20;

  // Title
  static constexpr const char* kTitle = "Page Buttons";
  if (title_font_.valid()) {
    const int tx = (W - text_w(title_font_, kTitle)) / 2;
    draw_text(buf, tx, y + title_font_.baseline(), kTitle, title_font_, false);
    y += title_font_.y_advance() + 6;
  }
  buf.fill_rect(0, y, W, 1, false);
  y += 14;

  if (step_ == 0) {
    if (body_font_.valid()) {
      static constexpr const char* kLines[] = {
        "Press the button you want",
        "for NEXT PAGE.",
        "(Left or Right front button)"
      };
      for (const char* line : kLines) {
        const int tx = (W - text_w(body_font_, line)) / 2;
        draw_text(buf, tx, y + body_font_.baseline(), line, body_font_, false);
        y += body_font_.y_advance() + 4;
      }
    }
    if (hint_font_.valid()) {
      static constexpr const char* kHint = "B0: Cancel";
      const int hx = (W - text_w(hint_font_, kHint)) / 2;
      draw_text(buf, hx, H - 20 + hint_font_.baseline(), kHint, hint_font_, false);
    }
  } else {
    if (body_font_.valid()) {
      const char* front_label = (pending_front_ == 2) ? "Next Page: Left button"
                                                       : "Next Page: Right button";
      const int fx = (W - text_w(body_font_, front_label)) / 2;
      draw_text(buf, fx, y + body_font_.baseline(), front_label, body_font_, false);
      y += body_font_.y_advance() + 12;

      static constexpr const char* kSideLabel = "Side Buttons:";
      draw_text(buf, 20, y + body_font_.baseline(), kSideLabel, body_font_, false);
      y += body_font_.y_advance() + 8;
    }

    static constexpr const char* kOptions[3] = {"Off", "Top = Next", "Bottom = Next"};
    const int row_h = hint_font_.valid() ? hint_font_.y_advance() + 10 : 24;
    for (int i = 0; i < 3; ++i) {
      const bool sel = (i == side_sel_);
      if (sel) buf.fill_rect(10, y, W - 20, row_h, false);  // black highlight
      if (hint_font_.valid()) {
        draw_text(buf, 24, y + 5 + hint_font_.baseline(), kOptions[i], hint_font_, sel);
      }
      y += row_h + 2;
    }

    if (hint_font_.valid()) {
      static constexpr const char* kHint = "B0: Back     B1: Done";
      const int hx = (W - text_w(hint_font_, kHint)) / 2;
      draw_text(buf, hx, H - 20 + hint_font_.baseline(), kHint, hint_font_, false);
    }
  }
}

void ButtonRemapScreen::start(DrawBuffer& buf, IRuntime&) {
  step_ = 0;
  pending_front_ = app_ ? app_->front_next_btn() : 2;
  side_sel_ = app_ ? static_cast<int>(app_->side_layout()) : 1;
  draw_(buf);
}

void ButtonRemapScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime&) {
  bool redraw = false;

  Button btn;
  while (buttons.next_press(btn)) {
    if (step_ == 0) {
      if (btn == Button::Button0) {
        if (app_) app_->pop_screen();
        return;
      }
      if (btn == Button::Button2 || btn == Button::Button3) {
        pending_front_ = (btn == Button::Button2) ? 2 : 3;
        step_ = 1;
        redraw = true;
        break;
      }
    } else {
      if (btn == Button::Button0) {
        step_ = 0;
        redraw = true;
        break;
      }
      if (btn == Button::Button1) {
        if (app_) {
          app_->set_front_next_btn(pending_front_);
          app_->set_side_layout(static_cast<SideLayout>(side_sel_));
          app_->pop_screen();
        }
        return;
      }
      if (btn == Button::Button3 || btn == Button::Down) {
        side_sel_ = (side_sel_ + 1) % 3;
        redraw = true;
      } else if (btn == Button::Button2 || btn == Button::Up) {
        side_sel_ = (side_sel_ + 2) % 3;
        redraw = true;
      }
    }
  }

  if (redraw) {
    draw_(buf);
    buf.refresh();
  }
}

}  // namespace microreader
