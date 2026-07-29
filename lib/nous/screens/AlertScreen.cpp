#include "AlertScreen.h"

#include <cstring>
#include <string>
#include <vector>

#include "../display/DrawBuffer.h"

namespace microreader {

static std::vector<std::string> alert_wrap_(const std::string& text, const BitmapFont& font, int max_w) {
  std::vector<std::string> lines;
  size_t i = 0, n = text.size();
  while (i < n) {
    size_t start = i, end = i;
    bool first = true;
    while (i < n) {
      while (i < n && text[i] == ' ') ++i;
      if (i >= n) break;
      size_t we = i;
      while (we < n && text[we] != ' ') ++we;
      if (static_cast<int>(font.word_width(text.c_str() + start, we - start, FontStyle::Regular)) <= max_w || first) {
        end = we; i = we; first = false;
      } else { break; }
    }
    lines.push_back(text.substr(start, end - start));
  }
  return lines;
}

void AlertScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t>) const {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  if (!ui_font_.valid())
    return;

  static constexpr int kLM = 16;
  static constexpr int kRM = 16;
  static constexpr int kLineGap = 3;
  const int text_max_w = W - kLM - kRM;

  const BitmapFont& title_f = header_font_.valid() ? header_font_ : ui_font_;
  const BitmapFont& body_f = ui_font_;

  int y = 24;

  // Title
  if (!title_.empty()) {
    buf.draw_text_proportional(kLM, y + static_cast<int>(title_f.baseline()),
                               title_.c_str(), title_f, false, FontStyle::Bold);
    y += static_cast<int>(title_f.y_advance()) + 6;
    buf.fill_rect(kLM, y, W - kLM - kRM, 1, false);
    y += 8;
  }

  // Body wrapped
  if (!body_.empty()) {
    const auto lines = alert_wrap_(body_, body_f, text_max_w);
    for (const auto& line : lines) {
      buf.draw_text_proportional(kLM, y + static_cast<int>(body_f.baseline()),
                                 line.c_str(), body_f, false, FontStyle::Regular);
      y += static_cast<int>(body_f.y_advance()) + kLineGap;
      if (y > H - 32) break;
    }
  }

  // Dismiss hint at bottom
  static const char kHint[] = "Press Back to dismiss";
  const int hint_w = static_cast<int>(body_f.word_width(kHint, sizeof(kHint) - 1, FontStyle::Regular));
  const int hint_y = H - static_cast<int>(body_f.y_advance()) - 10;
  buf.draw_text_proportional((W - hint_w) / 2, hint_y + static_cast<int>(body_f.baseline()),
                             kHint, body_f, false, FontStyle::Regular);
}

}  // namespace microreader
