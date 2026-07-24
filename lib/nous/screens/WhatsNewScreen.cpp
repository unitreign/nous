#include "WhatsNewScreen.h"

#include <cstring>
#include <string>
#include <vector>

#include "../version.h"
#include "../display/DrawBuffer.h"

namespace microreader {

static std::vector<std::string> wrap_text(const char* text, const BitmapFont& font,
                                          int max_w) {
  std::vector<std::string> lines;
  const std::string s(text);
  size_t i = 0, n = s.size();
  while (i < n) {
    size_t start = i, end = i;
    bool first = true;
    while (i < n) {
      while (i < n && s[i] == ' ') ++i;
      if (i >= n) break;
      size_t we = i;
      while (we < n && s[we] != ' ') ++we;
      if (static_cast<int>(font.word_width(s.c_str() + start, we - start, FontStyle::Regular)) <= max_w || first) {
        end = we; i = we; first = false;
      } else { break; }
    }
    lines.push_back(s.substr(start, end - start));
  }
  return lines;
}

void WhatsNewScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  const bool select_pressed =
      (buttons.pressed_latch >> static_cast<uint8_t>(Button::Button1)) & 1u;
  if (select_pressed) {
    page_ = 1 - page_;
    draw_all_(buf, std::nullopt);
    buf.full_refresh();
    return;
  }
  ListMenuScreen::update(buttons, buf, runtime);
}

void WhatsNewScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t>) const {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  if (!ui_font_.valid()) return;

  static constexpr int kLM      = 12;
  static constexpr int kRM      = 12;
  static constexpr int kLineGap = 2;
  static constexpr int kSecGap  = 10;
  static constexpr int kIndent  = 10;

  const BitmapFont& title_f   = header_font_.valid() ? header_font_ : ui_font_;
  const BitmapFont& body_f    = ui_font_;
  const BitmapFont& caption_f = subtitle_font_.valid() ? subtitle_font_ : ui_font_;
  const BitmapFont& sf        = section_font_.valid() ? section_font_ : caption_f;

  // ── Bottom tooltip boxes ──────────────────────────────────────────────
  static constexpr int kBotPad    = 5;
  static constexpr int kBotMargin = 10;
  static constexpr int kBoxLX     = 53;
  static constexpr int kBoxW      = 176;
  static constexpr int kLDiv      = kBoxLX + kBoxW / 2;

  const int box_h     = kBotPad + sf.y_advance() + kBotPad;
  const int bot_area  = 1 + box_h + kBotMargin;
  const int box_y     = H - bot_area;
  const int text_y    = box_y + kBotPad + sf.baseline();
  const int content_h = box_y;  // content must stay above this

  // Rule above tooltips
  buf.fill_rect(0, box_y, W, 1, false);

  auto draw_box = [&](int bx) {
    buf.fill_rect(bx,             box_y, kBoxW, 1,     false);
    buf.fill_rect(bx,             box_y + box_h - 1, kBoxW, 1, false);
    buf.fill_rect(bx,             box_y, 1,     box_h, false);
    buf.fill_rect(bx + kBoxW - 1, box_y, 1,     box_h, false);
  };
  draw_box(kBoxLX);
  buf.fill_rect(kLDiv, box_y, 1, box_h, false);

  const char* sel_label  = (page_ == 0) ? "2.2.0" : "2.2.1";
  const char* labels[2]  = {"Back", sel_label};
  const int   centers[2] = {kBoxLX + kBoxW / 4, kBoxLX + 3 * kBoxW / 4};
  for (int i = 0; i < 2; ++i) {
    const int tw = static_cast<int>(sf.word_width(labels[i], strlen(labels[i]), FontStyle::Regular));
    buf.draw_text_proportional(centers[i] - tw / 2, text_y,
                               labels[i], strlen(labels[i]), sf, false);
  }

  // ── Title row ─────────────────────────────────────────────────────────
  int y = 14;
  buf.draw_text_proportional(kLM, y + title_f.baseline(), "What's New", title_f, false);
  {
    const char* ver = (page_ == 0) ? MICROREADER_VERSION : "2.2.0";
    const int vw = static_cast<int>(caption_f.word_width(ver, strlen(ver), FontStyle::Regular));
    const int vy = y + title_f.y_advance() - caption_f.y_advance() + caption_f.baseline();
    buf.draw_text_proportional(W - kRM - vw, vy, ver, strlen(ver), caption_f, false);
  }
  y += title_f.y_advance() + 4;
  buf.fill_rect(0, y, W, 1, false);
  y += 8;

  // ── Helpers ───────────────────────────────────────────────────────────
  const int line_max_w = W - kLM - kRM;

  auto line = [&](const char* text, const BitmapFont& f, int indent = 0) {
    const auto wrapped = wrap_text(text, f, line_max_w - indent);
    for (const auto& wl : wrapped) {
      if (y + f.y_advance() > content_h) return;
      buf.draw_text_proportional(kLM + indent, y + f.baseline(),
                                 wl.c_str(), wl.size(), f, false);
      y += f.y_advance() + kLineGap;
    }
  };

  auto section = [&](const char* header) {
    if (y + caption_f.y_advance() > content_h) return;
    buf.draw_text_proportional(kLM, y + caption_f.baseline(),
                               header, strlen(header), caption_f, false);
    y += caption_f.y_advance() + 2;
    if (y < content_h) buf.fill_rect(kLM, y, W - kLM - kRM, 1, false);
    y += 5;
  };

  // ── Page content ──────────────────────────────────────────────────────
  if (page_ == 0) {
    section("NEW");
    line("Separate list and reader rotation settings",      body_f);
    line("Portrait / Landscape / Reversed rotation names",  body_f);
    line("Sleep screen: book cover + global stats image",   body_f);
    line("Global library stats screen",                     body_f);
    line("Book covers in two sizes (Lyra + sleep screen)",  body_f);
    line("Reworked Convert All Books screen",               body_f);
    line("Stats in Lyra, Lyra Ext, and all list themes",    body_f);
    line("Lyra tooltips in Settings, Books, Recent",        body_f);
    line("Consistent statusbar height in Lyra sub-menus",   body_f);
    line("Lyra header: nous and battery on same row",       body_f);
    line("Word count saved for reading-time estimates",     body_f);
    y += kSecGap;
    section("FIXED");
    line("Sleep screen: book stats image",                  body_f);
    line("Hidden books now shows Chapters option",          body_f);
    line("Battery setting hidden in Lyra themes",           body_f);
    line("Book options menu respects list rotation",        body_f);
  } else {
    section("NEW");
    line("Tabbed Settings: Look, Reader, Control, System", body_f);
    line("What's New screen on first boot after update",  body_f);
    line("nous logotype: custom embedded brand font",     body_f);
    line("Lyra & Lyra Ext default on fresh install",      body_f);
    line("Lyra: portrait-only, auto-corrects landscape",  body_f);
    line("Lyra: redesigned button tooltips",              body_f);
    line("Sleep Screen Text: show/hide title & author",   body_f);
    line("Convert All respects selected menu font size",  body_f);
    y += kSecGap;
    section("FIXED");
    line("Settings scroll on short tabs",                 body_f);
    line("Landscape auto-corrects on Lyra",               body_f);
    y += kSecGap;
    section("CHANGED");
    line("Minimal is now the default theme",              body_f);
    line("Theme names updated for clarity",               body_f);
    line("Literata replaces Bookerly",                    body_f);
    line("(drop .mfb in SD/fonts/ for custom)",          caption_f, kIndent);
  }
}

}  // namespace microreader
