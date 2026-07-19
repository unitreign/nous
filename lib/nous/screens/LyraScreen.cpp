#include "LyraScreen.h"

#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/BookIndex.h"

namespace microreader {

void LyraScreen::on_start() {
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
  idx_settings_     = i++; add_item("Settings");
}

void LyraScreen::on_select(int index) {
  if (!app_) return;
  if (index == idx_recent_ && has_recent_) {
    app_->record_book_opened(recent_path_);
    app_->reader()->set_path(recent_path_.c_str());
    app_->push_screen(ScreenId::Reader);
  } else if (index == idx_all_books_) {
    app_->push_screen(ScreenId::MainMenu);
  } else if (index == idx_recent_books_) {
    app_->push_screen(ScreenId::MainMenu);
  } else if (index == idx_settings_) {
    app_->push_screen(ScreenId::Settings);
  }
}

void LyraScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
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

void LyraScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  if (!ui_font_.valid()) return;
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  static constexpr int kPad      = 12;
  static constexpr int kCardPadV = 10;
  static constexpr int kSubGap   = 3;
  static constexpr int kNavPadV  = 8;
  static constexpr int kBotPad   = 5;

  const int ui_adv = ui_font_.y_advance();
  const int hf_adv = header_font_.valid() ? header_font_.y_advance() : ui_adv;
  const int sf_adv = section_font_.valid() ? section_font_.y_advance() : ui_adv;

  // ── Header ───────────────────────────────────────────────────────────────
  int y = 10;
  if (header_font_.valid())
    buf.draw_text_proportional(kPad, y + header_font_.baseline(), "NOUS", header_font_, false);
  else
    buf.draw_text_proportional(kPad, y + ui_font_.baseline(), "NOUS", ui_font_, false);

  if (battery_pct) {
    char pbuf[8];
    std::snprintf(pbuf, sizeof(pbuf), "%u%%", static_cast<unsigned>(*battery_pct));
    const BitmapFont& bf = section_font_.valid() ? section_font_ : ui_font_;
    const int pw = bf.word_width(pbuf, std::strlen(pbuf), FontStyle::Regular);
    buf.draw_text_proportional(W - kPad - pw, y + bf.baseline(), pbuf, bf, false);
  }
  y += hf_adv + 8;
  buf.fill_rect(0, y, W, 1, false);
  y += 1;

  // ── Bottom tooltip height (pre-compute so nav items don't overflow into it)
  const int bot_area_h = 1 + kBotPad + sf_adv + kBotPad;
  const int bot_rule_y = H - bot_area_h;

  // ── Recent book card ─────────────────────────────────────────────────────
  if (has_recent_ && idx_recent_ >= 0) {
    const int card_h = kCardPadV + ui_adv + kSubGap + sf_adv + kCardPadV;
    const bool sel = (selected() == idx_recent_);
    if (sel)
      buf.fill_rect(0, y, W, card_h, false);
    buf.draw_text_proportional(kPad, y + kCardPadV + ui_font_.baseline(),
                               recent_title_.c_str(), recent_title_.size(), ui_font_, sel);
    if (section_font_.valid() && !recent_author_.empty())
      buf.draw_text_proportional(kPad,
                                 y + kCardPadV + ui_adv + kSubGap + section_font_.baseline(),
                                 recent_author_.c_str(), recent_author_.size(), section_font_, sel);
    y += card_h;
    buf.fill_rect(0, y, W, 1, false);
    y += 1;
  }

  // ── Nav items ─────────────────────────────────────────────────────────────
  const int nav_row_h = kNavPadV + ui_adv + kNavPadV;
  struct NavItem { int idx; const char* label; };
  const NavItem nav[] = {
    {idx_all_books_,    "All Books"},
    {idx_recent_books_, "Recent Books"},
    {idx_settings_,     "Settings"},
  };
  for (const auto& item : nav) {
    if (item.idx < 0) continue;
    const bool sel = (selected() == item.idx);
    if (sel)
      buf.fill_rect(0, y, W, nav_row_h, false);
    buf.draw_text_proportional(kPad, y + kNavPadV + ui_font_.baseline(), item.label, ui_font_, sel);
    y += nav_row_h;
    if (y < bot_rule_y) {
      buf.fill_rect(0, y, W, 1, false);
      y += 1;
    }
  }

  // ── Bottom tooltip ────────────────────────────────────────────────────────
  buf.fill_rect(0, bot_rule_y, W, 1, false);
  if (section_font_.valid()) {
    const BitmapFont& sf = section_font_;
    const char* labels[] = {"Up", "Down", "Select", "Back"};
    static constexpr int kN = 4;
    int ws[kN];
    int total_w = 0;
    for (int i = 0; i < kN; ++i) {
      ws[i] = sf.word_width(labels[i], std::strlen(labels[i]), FontStyle::Regular);
      total_w += ws[i];
    }
    const int gap = (W - 2 * kPad - total_w) / (kN - 1);
    int hx = kPad;
    const int ty = bot_rule_y + 1 + kBotPad + sf.baseline();
    for (int i = 0; i < kN; ++i) {
      buf.draw_text_proportional(hx, ty, labels[i], sf, false);
      hx += ws[i] + gap;
    }
  }
}

}  // namespace microreader
