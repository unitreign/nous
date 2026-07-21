#include "LyraExtScreen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/Book.h"
#include "../content/BookIndex.h"

namespace microreader {

// Cover mode: scale src so it fills dst_w × dst_h, centered, crop overflow.
// Maintains aspect ratio — one dimension fills exactly, the other is cropped.
// Example: src 300×400, dst 200×100 → scale by width (200/300), rendering
// 200×267 then cropping to the center 100 rows.
static void blit_cover(DrawBuffer& buf, int dst_x, int dst_y,
                        const uint8_t* data, int src_w, int src_h,
                        int dst_w, int dst_h) {
  if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
  const int src_stride = (src_w + 7) / 8;
  const int dst_stride = (dst_w + 7) / 8;
  uint8_t row_buf[80];  // supports up to 640px output columns
  if (dst_stride > (int)sizeof(row_buf)) return;

  // Determine dominant axis: if scaling by width already fills height → width-dominant.
  // width-dominant: dst_w * src_h >= dst_h * src_w
  const bool wd = (dst_w * src_h >= dst_h * src_w);

  // Pre-compute the crop offset in the SCALED space.
  // width-dominant: scale_factor = dst_w / src_w
  //   scaled_h = src_h * dst_w / src_w
  //   crop_y = (scaled_h - dst_h) / 2  (in scaled pixels, maps to src via ×src_w/dst_w)
  // height-dominant: scale_factor = dst_h / src_h
  //   scaled_w = src_w * dst_h / src_h
  //   crop_x = (scaled_w - dst_w) / 2
  const int crop_y = wd ? ((src_h * dst_w / src_w) - dst_h) / 2 : 0;
  const int crop_x = wd ? 0 : ((src_w * dst_h / src_h) - dst_w) / 2;

  for (int dy = 0; dy < dst_h; ++dy) {
    // Source row index using nearest-neighbour.
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

// Draw a 1px border outline around a rectangle.
static void draw_outline(DrawBuffer& buf, int x, int y, int w, int h) {
  buf.fill_rect(x,         y,         w, 1, false);
  buf.fill_rect(x,         y + h - 1, w, 1, false);
  buf.fill_rect(x,         y,         1, h, false);
  buf.fill_rect(x + w - 1, y,         1, h, false);
}

void LyraExtScreen::load_cover_(int i) {
  auto& s = slots_[i];
  s.cover_data.clear();
  s.cover_loaded = false;
  s.cover_w = s.cover_h = 0;
  if (s.bin_path.empty()) return;
  FILE* f = std::fopen(s.bin_path.c_str(), "rb");
  if (!f) return;
  uint16_t hdr[2] = {};
  if (std::fread(hdr, 2, 2, f) != 2) { std::fclose(f); return; }
  s.cover_w = hdr[0];
  s.cover_h = hdr[1];
  const size_t stride = (s.cover_w + 7) / 8;
  const size_t data_sz = stride * s.cover_h;
  if (data_sz == 0 || data_sz > 8192) { std::fclose(f); return; }
  s.cover_data.resize(data_sz);
  if (std::fread(s.cover_data.data(), 1, data_sz, f) == data_sz)
    s.cover_loaded = true;
  std::fclose(f);
}

void LyraExtScreen::on_start() {
  // Lyra Ext is portrait-only; clamp landscape rotations to portrait and persist it.
  const Rotation rot = current_rotation_();
  if (rot == Rotation::Deg0 || rot == Rotation::Deg180) {
    set_buf_rotation_(Rotation::Deg90);
    if (app_) app_->set_rotate_display(0);  // 0 = Portrait (Deg90)
  }

  if (app_ && app_->data_dir_) {
    const std::string idx_path = std::string(app_->data_dir_) + "/book_index.dat";
    BookIndex::instance().load(idx_path);
    app_->synchronize_reader_recents();
  }

  num_books_ = 0;
  for (int i = 0; i < kMaxBooks; ++i) {
    slots_[i] = BookSlot{};
    idx_books_[i] = -1;
  }

  // Collect top-3 most recently opened books.
  struct Raw { uint32_t order; std::string path; std::string title; };
  std::vector<Raw> raw;
  const StringPool& pool = BookIndex::instance().pool();
  for (const auto& e : BookIndex::instance().entries()) {
    if (e.last_open_order == 0) continue;
    raw.push_back({e.last_open_order, e.path.to_string(pool), std::string(e.title.view(pool))});
  }
  std::stable_sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) {
    return a.order > b.order;
  });

  num_books_ = static_cast<int>(std::min(raw.size(), static_cast<size_t>(kMaxBooks)));

  clear_items();
  int item_idx = 0;
  for (int i = 0; i < num_books_; ++i) {
    idx_books_[i] = item_idx++;
    add_item(raw[i].title);
    slots_[i].path  = raw[i].path;
    slots_[i].title = raw[i].title;
  }
  // Fill remaining book indices with -1 (already set above).

  idx_all_books_    = item_idx++; add_item("All Books");
  idx_recent_books_ = item_idx++; add_item("Recent Books");
  idx_settings_     = item_idx++; add_item("Settings");

  // Load or flag covers.
  for (int i = 0; i < num_books_; ++i) {
    auto& s = slots_[i];
    if (app_ && app_->data_dir_) {
      s.bin_path = cover_bin_path(s.path.c_str(), app_->data_dir_);
      FILE* chk = std::fopen(s.bin_path.c_str(), "rb");
      if (chk) {
        std::fclose(chk);
        load_cover_(i);
      } else {
        s.cover_needs_extract = true;
      }
    }
  }
}

void LyraExtScreen::on_select(int index) {
  if (!app_) return;
  for (int i = 0; i < num_books_; ++i) {
    if (index == idx_books_[i]) {
      app_->record_book_opened(slots_[i].path);
      app_->ensure_cover_bin(slots_[i].path);
      app_->reader()->set_path(slots_[i].path.c_str());
      app_->push_screen(ScreenId::Reader);
      return;
    }
  }
  if (index == idx_all_books_)    app_->push_screen(ScreenId::MainMenu);
  else if (index == idx_recent_books_) app_->push_screen(ScreenId::RecentBooks);
  else if (index == idx_settings_)     app_->push_screen(ScreenId::Settings);
}

void LyraExtScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  // Lazy cover extraction: handle one pending slot per frame.
  for (int i = 0; i < num_books_; ++i) {
    if (slots_[i].cover_needs_extract) {
      slots_[i].cover_needs_extract = false;
      if (app_) app_->ensure_cover_bin(slots_[i].path);
      load_cover_(i);
      request_redraw();
      return;
    }
  }

  const bool back_down = buttons.is_down(Button::Button0);
  ButtonState fwd = buttons;
  if (back_down) {
    if (back_hold_frames_ <= kHiddenHoldFrames) back_hold_frames_++;
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

void LyraExtScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  if (!ui_font_.valid()) return;
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  // All spacing constants match LyraScreen exactly.
  static constexpr int kPad         = 12;
  static constexpr int kTopGap      = 36;
  static constexpr int kSlotGap     = 8;    // gap between the 3 cover slots
  static constexpr int kCoverTitleGap = 4;
  static constexpr int kCardNavGap  = 24;
  static constexpr int kNavPadV     = 32;
  static constexpr int kBotPad      = 5;
  static constexpr int kBotMargin   = 10;

  const int ui_adv = ui_font_.y_advance();
  const int hf_adv = header_font_.valid() ? header_font_.y_advance() : ui_adv;
  const int sf_adv = section_font_.valid() ? section_font_.y_advance() : ui_adv;

  // ── Header ───────────────────────────────────────────────────────────────
  int y = 10;
  {
    const BitmapFont& brand_f = brand_font_.valid() ? brand_font_ : ui_font_;
    buf.draw_text_proportional(kPad, y + brand_f.baseline(), "nous", brand_f, false);
  }

  if (battery_pct) {
    char pbuf[8];
    std::snprintf(pbuf, sizeof(pbuf), "%u%%", static_cast<unsigned>(*battery_pct));
    const BitmapFont& bf = section_font_.valid() ? section_font_ : ui_font_;
    const int pw = bf.word_width(pbuf, std::strlen(pbuf), FontStyle::Regular);
    buf.draw_text_proportional(W - kPad - pw, y + bf.baseline(), pbuf, bf, false);
  }
  y += hf_adv + 8;
  buf.fill_rect(0, y, W, 1, false);
  y += 1 + kTopGap;

  // ── Bottom tooltip height pre-compute ────────────────────────────────────
  const int bot_area_h = 1 + kBotPad + sf_adv + kBotPad + kBotMargin;
  const int bot_rule_y = H - bot_area_h;

  // ── 3 cover slots ────────────────────────────────────────────────────────
  const int slot_w        = (W - 2 * kPad - 2 * kSlotGap) / 3;
  const int cover_slot_h  = slot_w * 3 / 2;  // max slot height (3:2 portrait box)
  const int title_area_h  = ui_adv;
  const int slot_total_h  = cover_slot_h + kCoverTitleGap + title_area_h;

  static const char kEll[] = "...";

  for (int i = 0; i < kMaxBooks; ++i) {
    const int slot_x = kPad + i * (slot_w + kSlotGap);
    const bool sel = (idx_books_[i] >= 0 && selected() == idx_books_[i]);

    if (sel)
      buf.fill_rect(slot_x - 1, y - 1, slot_w + 2, slot_total_h + 2, false);

    if (i < num_books_) {
      const auto& s = slots_[i];
      if (s.cover_loaded && s.cover_w > 0 && s.cover_h > 0) {
        // Cover mode: fill slot_w × cover_slot_h, centered, crop overflow edges.
        blit_cover(buf, slot_x, y,
                   s.cover_data.data(), s.cover_w, s.cover_h,
                   slot_w, cover_slot_h);
      } else {
        // Placeholder outline
        draw_outline(buf, slot_x, y, slot_w, cover_slot_h);
      }

      // Truncated title below cover
      const int title_y = y + cover_slot_h + kCoverTitleGap + ui_font_.baseline();
      const std::string_view title_sv = s.title;
      const int max_tw = slot_w;
      int tw = ui_font_.word_width(title_sv.data(), title_sv.size(), FontStyle::Regular);
      if (tw <= max_tw) {
        buf.draw_text_proportional(slot_x, title_y, title_sv.data(), title_sv.size(), ui_font_, sel);
      } else {
        // Truncate with ellipsis
        const int ell_w = ui_font_.word_width(kEll, 3, FontStyle::Regular);
        const int budget = max_tw - ell_w;
        size_t fit = 0;
        const char* p = title_sv.data();
        while (*p) {
          const uint8_t b = static_cast<uint8_t>(*p);
          const size_t cb = b < 0x80 ? 1u : b < 0xE0 ? 2u : b < 0xF0 ? 3u : 4u;
          if (ui_font_.word_width(title_sv.data(), fit + cb, FontStyle::Regular) > budget) break;
          fit += cb; p += cb;
        }
        char trunc[260];
        const size_t cp = fit < 256 ? fit : 256;
        std::memcpy(trunc, title_sv.data(), cp);
        std::memcpy(trunc + cp, kEll, 3);
        trunc[cp + 3] = '\0';
        buf.draw_text_proportional(slot_x, title_y, trunc, cp + 3, ui_font_, sel);
      }
    } else {
      // Empty slot outline
      draw_outline(buf, slot_x, y, slot_w, cover_slot_h);
    }
  }

  y += slot_total_h + kCardNavGap;

  // ── Nav items ────────────────────────────────────────────────────────────
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
      buf.fill_rect(bx,             box_y,             kBoxW, 1,     false);
      buf.fill_rect(bx,             box_y + box_h - 1, kBoxW, 1,     false);
      buf.fill_rect(bx,             box_y,             1,     box_h, false);
      buf.fill_rect(bx + kBoxW - 1, box_y,             1,     box_h, false);
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
