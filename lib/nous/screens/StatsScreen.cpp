#include "StatsScreen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/Book.h"
#include "../display/DrawBuffer.h"
#include "../display/ui_font_large.h"    // 24px Inter (for title_font_)

namespace microreader {

// Scale src to fit dst_w × dst_h exactly (blit_cover already handles crop/scale).
static void blit_cover(DrawBuffer& buf, int dst_x, int dst_y,
                       const uint8_t* data, int src_w, int src_h,
                       int dst_w, int dst_h) {
  if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
  const int src_stride = (src_w + 7) / 8;
  const int dst_stride = (dst_w + 7) / 8;
  uint8_t row_buf[80];
  if (dst_stride > static_cast<int>(sizeof(row_buf))) return;
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

// Word-wrap text into lines that fit within max_w pixels.
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
      const int w = static_cast<int>(font.word_width(text.c_str() + line_start,
                                                      word_end - line_start, style));
      if (w <= max_w || first_word) {
        line_end   = word_end;
        i          = word_end;
        first_word = false;
      } else {
        break;
      }
    }
    lines.push_back(text.substr(line_start, line_end - line_start));
  }
  return lines;
}

void StatsScreen::on_start() {
  // Portrait-only: clamp landscape to portrait.
  const Rotation rot = current_rotation_();
  if (rot == Rotation::Deg0 || rot == Rotation::Deg180) {
    set_buf_rotation_(Rotation::Deg90);
    if (app_) app_->set_rotate_display(0);
  }

  // Init 24px title font.
  title_font_ = BitmapFont{};
  title_font_.init(kFontData_ui_large_mbf, kFontData_ui_large_mbf_size);

  // Load cover from .bin cache, scaled to the thumbnail display width.
  cover_loaded_ = false;
  cover_data_.clear();
  cover_w_ = cover_h_ = 0;
  if (!book_path_.empty() && app_ && app_->data_dir_) {
    const std::string cpath = cover_bin_path(book_path_.c_str(), app_->data_dir_);
    FILE* f = std::fopen(cpath.c_str(), "rb");
    if (f) {
      uint16_t hdr[2] = {};
      if (std::fread(hdr, 2, 2, f) == 2) {
        const int src_w = hdr[0], src_h = hdr[1];
        if (src_w > 0 && src_h > 0) {
          static constexpr int kThumbW = 80;
          const int dst_w = std::min(src_w, kThumbW);
          const int dst_h = dst_w * src_h / src_w;
          const int src_stride = (src_w + 7) / 8;
          const int dst_stride = (dst_w + 7) / 8;
          cover_data_.assign(static_cast<size_t>(dst_stride) * dst_h, 0xFF);
          std::vector<uint8_t> src_row(src_stride);
          int prev_sy = -1;
          bool ok = true;
          for (int dy = 0; dy < dst_h; ++dy) {
            const int sy = dy * src_h / dst_h;
            if (sy != prev_sy) {
              std::fseek(f, 4 + static_cast<long>(sy) * src_stride, SEEK_SET);
              if (std::fread(src_row.data(), 1, src_stride, f) != static_cast<size_t>(src_stride))
                { ok = false; break; }
              prev_sy = sy;
            }
            uint8_t* dr = cover_data_.data() + static_cast<size_t>(dy) * dst_stride;
            for (int dx = 0; dx < dst_w; ++dx) {
              const int sx = dx * src_w / dst_w;
              if (!((src_row[sx >> 3] >> (7 - (sx & 7))) & 1))
                dr[dx >> 3] &= static_cast<uint8_t>(~(1u << (7 - (dx & 7))));
            }
          }
          if (ok) {
            cover_w_ = static_cast<uint16_t>(dst_w);
            cover_h_ = static_cast<uint16_t>(dst_h);
            cover_loaded_ = true;
          } else {
            cover_data_.clear();
          }
        }
      }
      std::fclose(f);
    }
  }
}

void StatsScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t>) const {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  if (!subtitle_font_.valid()) return;

  // Font tiers:
  //   hf32 = 32px  (header_font_) — big progress %
  //   tf24 = 24px  (title_font_)  — "Statistics", book title, author, KV values
  //   vf18 = 18px  (ui_font_)     — chapter title, Ch.X of Y, "book complete",
  //                                  chapter %, KV labels
  //   sf14 = 14px  (subtitle_font_) — bottom tooltip labels only
  const BitmapFont& hf32 = header_font_.valid() ? header_font_ : ui_font_;
  const BitmapFont& tf24 = title_font_.valid()  ? title_font_  : ui_font_;
  const BitmapFont& vf18 = ui_font_;
  const BitmapFont& sf14 = subtitle_font_;

  static constexpr int kLM = 16;
  static constexpr int kRM = 16;
  const int inner_w = W - kLM - kRM;

  int y = 12;

  // ── "Statistics" header + 2px divider ───────────────────────────────────
  buf.draw_text_proportional(kLM, y + tf24.baseline(), "Statistics", 10, tf24, false);
  y += tf24.y_advance() + 3;
  buf.fill_rect(0, y, W, 2, false);
  y += 2 + 14;

  // ── Top card: cover (left, aspect-ratio correct) + book info (right) ────
  {
    static constexpr int kCardPad  = 10;
    static constexpr int kCoverGap = 12;
    static constexpr int kMaxCoverW = 80;   // max rendered cover width
    static constexpr int kLineGap   = 6;

    // ── Compute scaled cover size (preserve aspect ratio, max kMaxCoverW wide)
    int scaled_cov_w = 0, scaled_cov_h = 0;
    if (cover_loaded_ && cover_w_ > 0 && cover_h_ > 0) {
      scaled_cov_w = std::min(static_cast<int>(cover_w_), kMaxCoverW);
      scaled_cov_h = scaled_cov_w * static_cast<int>(cover_h_) / static_cast<int>(cover_w_);
    }

    // Text column: fixed slot based on scaled cover width
    const int cover_col_w = scaled_cov_w > 0 ? scaled_cov_w + kCoverGap : 0;
    const int text_x      = kLM + cover_col_w;
    const int text_max_w  = W - text_x - kRM;

    // Build uppercase title and word-wrap it (only field that wraps)
    std::string uc = book_title_;
    for (auto& c : uc) if (c >= 'a' && c <= 'z') c -= 32;
    const auto title_lines = wrap_text(uc, tf24, text_max_w, FontStyle::Regular);
    const int n_title = static_cast<int>(title_lines.size());

    // Truncate single-line fields (author, chapter title)
    auto truncate = [&](const std::string& s, const BitmapFont& f, FontStyle fs) -> std::string {
      std::string out = s;
      while (!out.empty() &&
             f.word_width(out.c_str(), out.size(), fs) > static_cast<uint32_t>(text_max_w)) {
        if (out.size() > 3) { out.resize(out.size() - 1); out.back() = '.'; }
        else break;
      }
      return out;
    };
    const std::string auth    = truncate(author_,        vf18, FontStyle::Regular);
    const std::string chtitle = truncate(chapter_title_, vf18, FontStyle::Italic);

    // Chapter label: "Chapter title  ·  Ch. X of Y"
    char ch_buf[64];
    if (chapter_count_ > 1)
      std::snprintf(ch_buf, sizeof(ch_buf), "Ch. %d of %d", chapter_idx_ + 1, chapter_count_);
    else
      std::snprintf(ch_buf, sizeof(ch_buf), "Chapter %d", chapter_idx_ + 1);

    // Measure text block height
    int text_block_h = n_title * tf24.y_advance();
    if (!auth.empty())    text_block_h += kLineGap + vf18.y_advance();
    if (!chtitle.empty()) text_block_h += kLineGap + vf18.y_advance();
    text_block_h += kLineGap + vf18.y_advance();  // Ch. X of Y always shown

    // Card height: tallest of text block or cover (both with padding)
    const int text_card_h  = kCardPad + text_block_h + kCardPad;
    const int cover_card_h = scaled_cov_h > 0 ? scaled_cov_h + kCardPad * 2 : 0;
    const int card_h       = std::max(text_card_h, cover_card_h);

    // ── Draw cover, centred vertically in its slot
    if (scaled_cov_w > 0) {
      const int slot_h    = card_h - kCardPad * 2;
      const int cov_y_off = (slot_h - scaled_cov_h) / 2;
      blit_cover(buf, kLM, y + kCardPad + cov_y_off,
                 cover_data_.data(), cover_w_, cover_h_, scaled_cov_w, scaled_cov_h);
    }

    // ── Draw text block, centred vertically in card
    int ty = y + (card_h - text_block_h) / 2;

    for (int li = 0; li < n_title; ++li) {
      buf.draw_text_proportional(text_x, ty + tf24.baseline(),
                                 title_lines[li].c_str(), title_lines[li].size(), tf24, false);
      ty += tf24.y_advance();
    }
    if (!auth.empty()) {
      ty += kLineGap;
      buf.draw_text_proportional(text_x, ty + tf24.baseline(),
                                 auth.c_str(), auth.size(), tf24, false);
      ty += tf24.y_advance();
    }
    if (!chtitle.empty()) {
      ty += kLineGap;
      buf.draw_text_proportional(text_x, ty + vf18.baseline(),
                                 chtitle.c_str(), chtitle.size(), vf18, false,
                                 FontStyle::Italic);
      ty += vf18.y_advance();
    }
    ty += kLineGap;
    buf.draw_text_proportional(text_x, ty + vf18.baseline(),
                               ch_buf, std::strlen(ch_buf), vf18, false);

    y += card_h + 16;
  }

  // ── Progress section ─────────────────────────────────────────────────────
  {
    static constexpr int kBarH    = 6;
    static constexpr int kBarInner = kBarH - 2;

    // Big book % (32px, centred)
    char pct_buf[8];
    std::snprintf(pct_buf, sizeof(pct_buf), "%d%%", progress_pct_);
    {
      const int pw = static_cast<int>(hf32.word_width(pct_buf, std::strlen(pct_buf), FontStyle::Regular));
      buf.draw_text_proportional((W - pw) / 2, y + hf32.baseline(), pct_buf, std::strlen(pct_buf), hf32, false);
    }
    y += hf32.y_advance() + 4;

    // "book complete" (18px, centred)
    {
      static const char kCaption[] = "book complete";
      const int cw = static_cast<int>(vf18.word_width(kCaption, sizeof(kCaption) - 1, FontStyle::Regular));
      buf.draw_text_proportional((W - cw) / 2, y + vf18.baseline(), kCaption, sizeof(kCaption) - 1, vf18, false);
    }
    y += vf18.y_advance() + 10;

    // Book progress bar
    buf.fill_rect(kLM, y, inner_w, kBarH, false);
    buf.fill_rect(kLM + 1, y + 1, inner_w - 2, kBarInner, true);
    if (progress_pct_ > 0) {
      const int filled = (inner_w - 2) * progress_pct_ / 100;
      if (filled > 0) buf.fill_rect(kLM + 1, y + 1, filled, kBarInner, false);
    }
    y += kBarH + 8;

    // Chapter % label (18px, right-aligned)
    {
      char cpct[20];
      std::snprintf(cpct, sizeof(cpct), "Ch. %d%%", chapter_progress_pct_);
      const int cw = static_cast<int>(vf18.word_width(cpct, std::strlen(cpct), FontStyle::Regular));
      buf.draw_text_proportional(W - kRM - cw, y + vf18.baseline(), cpct, std::strlen(cpct), vf18, false);
    }
    y += vf18.y_advance() + 4;

    // Chapter progress bar
    buf.fill_rect(kLM, y, inner_w, kBarH, false);
    buf.fill_rect(kLM + 1, y + 1, inner_w - 2, kBarInner, true);
    if (chapter_progress_pct_ > 0) {
      const int filled = (inner_w - 2) * chapter_progress_pct_ / 100;
      if (filled > 0) buf.fill_rect(kLM + 1, y + 1, filled, kBarInner, false);
    }
    y += kBarH + 20;
  }

  // Thin divider before key-value section
  buf.fill_rect(kLM, y, inner_w, 1, false);
  y += 14;

  // ── Key-value rows (18px label / 24px value, two columns) ────────────────
  {
    const int col_w  = inner_w / 2;
    const int row_h  = vf18.y_advance() + 3 + tf24.y_advance();
    const int row_gap = 18;

    char rt_buf[24], tl_buf[24], to_buf[12], pt_buf[12], ppm_buf[20], avg_buf[24];

    {
      const uint64_t mins = reading_ms_ / 60000ULL;
      const uint64_t hrs  = mins / 60ULL;
      const uint64_t mn   = mins % 60ULL;
      if (hrs > 0)
        std::snprintf(rt_buf, sizeof(rt_buf), "%uh %02um",
                      static_cast<unsigned>(hrs), static_cast<unsigned>(mn));
      else
        std::snprintf(rt_buf, sizeof(rt_buf), "%um", static_cast<unsigned>(mins));
    }

    if (time_left_ms_ > 0) {
      const uint64_t mins = time_left_ms_ / 60000ULL;
      const uint64_t hrs  = mins / 60ULL;
      const uint64_t mn   = mins % 60ULL;
      if (hrs > 0)
        std::snprintf(tl_buf, sizeof(tl_buf), "~%uh %02um",
                      static_cast<unsigned>(hrs), static_cast<unsigned>(mn));
      else
        std::snprintf(tl_buf, sizeof(tl_buf), "~%um", static_cast<unsigned>(mins));
    } else {
      std::snprintf(tl_buf, sizeof(tl_buf), "\xe2\x80\x94");
    }

    std::snprintf(to_buf, sizeof(to_buf), "%u", static_cast<unsigned>(times_opened_));
    std::snprintf(pt_buf, sizeof(pt_buf), "%u", static_cast<unsigned>(page_turns_));

    if (page_turns_ > 0 && reading_ms_ > 0) {
      const uint64_t x10 = (static_cast<uint64_t>(page_turns_) * 600000ULL) / reading_ms_;
      std::snprintf(ppm_buf, sizeof(ppm_buf), "%llu.%llu/min",
                    static_cast<unsigned long long>(x10 / 10),
                    static_cast<unsigned long long>(x10 % 10));
    } else {
      std::snprintf(ppm_buf, sizeof(ppm_buf), "\xe2\x80\x94");
    }

    {
      const uint64_t opened   = times_opened_ > 0 ? times_opened_ : 1;
      const uint64_t avg_mins = (reading_ms_ / opened) / 60000ULL;
      if (avg_mins >= 60)
        std::snprintf(avg_buf, sizeof(avg_buf), "%uh %02um",
                      static_cast<unsigned>(avg_mins / 60),
                      static_cast<unsigned>(avg_mins % 60));
      else
        std::snprintf(avg_buf, sizeof(avg_buf), "%um", static_cast<unsigned>(avg_mins));
    }

    auto draw_kv = [&](int col, int top_y, const char* label, const char* value) {
      const int x = kLM + col * col_w;
      buf.draw_text_proportional(x, top_y + vf18.baseline(),
                                 label, std::strlen(label), vf18, false);
      buf.draw_text_proportional(x, top_y + vf18.y_advance() + 3 + tf24.baseline(),
                                 value, std::strlen(value), tf24, false);
    };

    draw_kv(0, y, "Reading Time", rt_buf);
    draw_kv(1, y, "Time Left",    tl_buf);
    y += row_h + row_gap;

    draw_kv(0, y, "Times Opened", to_buf);
    draw_kv(1, y, "Avg. Session", avg_buf);
    y += row_h + row_gap;

    draw_kv(0, y, "Page Turns", pt_buf);
    draw_kv(1, y, "Pages/min",  ppm_buf);
  }

  // ── Bottom tooltip boxes (Lyra-identical, no divider above) ─────────────
  {
    const BitmapFont& sf = section_font_.valid() ? section_font_ : sf14;
    const bool inv = app_ && app_->invert_menu_buttons();

    static constexpr int kBotPad    = 5;
    static constexpr int kBotMargin = 10;
    static constexpr int kBoxLX     = 53;
    static constexpr int kBoxW      = 176;
    static constexpr int kBoxRX     = kBoxLX + kBoxW + 22;
    static constexpr int kLDiv      = kBoxLX + kBoxW / 2;
    static constexpr int kRDiv      = kBoxRX + kBoxW / 2;

    const int sf_adv     = sf.y_advance();
    const int box_h      = kBotPad + sf_adv + kBotPad;
    const int box_y      = H - (1 + kBotPad + sf_adv + kBotPad + kBotMargin);
    const int text_y     = box_y + kBotPad + sf.baseline();

    auto draw_box = [&](int bx) {
      buf.fill_rect(bx,              box_y,             kBoxW, 1,     false);
      buf.fill_rect(bx,              box_y + box_h - 1, kBoxW, 1,     false);
      buf.fill_rect(bx,              box_y,             1,     box_h, false);
      buf.fill_rect(bx + kBoxW - 1,  box_y,             1,     box_h, false);
    };
    draw_box(kBoxLX);
    buf.fill_rect(kLDiv, box_y, 1, box_h, false);
    draw_box(kBoxRX);
    buf.fill_rect(kRDiv, box_y, 1, box_h, false);

    const char* labels[4] = {"Back", "\xe2\x80\x94", inv ? "Up" : "Down", inv ? "Down" : "Up"};
    const int   centers[4] = {kBoxLX + kBoxW / 4, kBoxLX + 3 * kBoxW / 4,
                               kBoxRX + kBoxW / 4, kBoxRX + 3 * kBoxW / 4};
    for (int i = 0; i < 4; ++i) {
      const int tw = static_cast<int>(sf.word_width(labels[i], std::strlen(labels[i]), FontStyle::Regular));
      buf.draw_text_proportional(centers[i] - tw / 2, text_y,
                                 labels[i], std::strlen(labels[i]), sf, false);
    }
  }
}

}  // namespace microreader
