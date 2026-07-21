#include "WhatsNewScreen.h"

#include <cstring>

#include "../version.h"
#include "../display/DrawBuffer.h"

namespace microreader {

void WhatsNewScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t>) const {
  const int W = buf.width();
  buf.fill(true);

  if (!ui_font_.valid()) return;

  static constexpr int kLM        = 12;
  static constexpr int kRM        = 12;
  static constexpr int kLineGap   = 2;
  static constexpr int kSectionGap = 8;
  static constexpr int kIndent    = 10;

  int y = 14;

  // ── Title ──────────────────────────────────────────────────────────────
  const BitmapFont& title_f = header_font_.valid() ? header_font_ : ui_font_;
  buf.draw_text_proportional(kLM, y + title_f.baseline(), "What's New", title_f, false);

  // Version right-aligned, bottom-aligned with title
  if (subtitle_font_.valid()) {
    const char* ver = MICROREADER_VERSION;
    const int vw = static_cast<int>(subtitle_font_.word_width(ver, strlen(ver), FontStyle::Regular));
    const int vy = y + title_f.y_advance() - subtitle_font_.y_advance() + subtitle_font_.baseline();
    buf.draw_text_proportional(W - kRM - vw, vy, ver, strlen(ver), subtitle_font_, false);
  }

  y += title_f.y_advance() + 4;
  buf.fill_rect(0, y, W, 1, false);
  y += 8;

  // ── Helpers ────────────────────────────────────────────────────────────
  const BitmapFont& body_f    = ui_font_;
  const BitmapFont& caption_f = subtitle_font_.valid() ? subtitle_font_ : ui_font_;

  auto line = [&](const char* text, const BitmapFont& f, int indent = 0) {
    buf.draw_text_proportional(kLM + indent, y + f.baseline(), text, strlen(text), f, false);
    y += f.y_advance() + kLineGap;
  };

  auto section = [&](const char* header) {
    buf.draw_text_proportional(kLM, y + caption_f.baseline(), header, strlen(header), caption_f, false);
    y += caption_f.y_advance() + 2;
    buf.fill_rect(kLM, y, W - kLM - kRM, 1, false);
    y += 5;
  };

  // ── New ────────────────────────────────────────────────────────────────
  section("NEW");
  line("Tabbed Settings",                   body_f);
  line("What's New screen",                 body_f);
  line("nous logotype font",                body_f);
  line("Refined Menu Font (Inter)",         body_f);
  line("Sleep Screen Text Toggle",          body_f);
  line("Convert All respects menu font",    body_f);
  line("Lyra themes: portrait-only layout", body_f);
  line("Lyra themes: redesigned tooltips",  body_f);

  y += kSectionGap;

  // ── Fixed ──────────────────────────────────────────────────────────────
  section("FIXED");
  line("Settings scroll on short tabs",     body_f);
  line("Landscape auto-corrects on Lyra",   body_f);

  y += kSectionGap;

  // ── Changed ────────────────────────────────────────────────────────────
  section("CHANGED");
  line("Minimal is now the default theme",  body_f);
  line("Theme names updated for clarity",   body_f);
  line("Literata replaces Bookerly",        body_f);
  line("(drop .mfb in SD/fonts/ for custom)", caption_f, kIndent);
}

}  // namespace microreader
