#pragma once

#include <cstdint>

#include "ContentModel.h"

namespace microreader {

// ---------------------------------------------------------------------------
// Font measurement interface — abstract so tests can use a fixed-width font
// ---------------------------------------------------------------------------

struct IFont {
  virtual ~IFont() = default;

  // Width of a single character in pixels. Returns 0 if glyph missing.
  virtual uint16_t char_width(char32_t ch, FontStyle style, uint8_t size_pct = 100) const = 0;

  // Width of a UTF-8 word (sum of char widths)
  virtual uint16_t word_width(const char* text, size_t len, FontStyle style, uint8_t size_pct = 100) const = 0;

  // Vertical advance per line (includes leading)
  virtual uint16_t y_advance(uint8_t size_pct = 100) const = 0;

  // Distance from top of line to baseline (where text sits, above descenders)
  virtual uint16_t baseline(uint8_t size_pct = 100) const = 0;

  // Distance below baseline to underline top (pixels, positive = below baseline)
  virtual int8_t underline_pos(uint8_t size_pct = 100) const {
    return 1;
  }

  // Underline height in pixels (>= 1)
  virtual uint8_t underline_thickness(uint8_t size_pct = 100) const {
    return 1;
  }
};

// ---------------------------------------------------------------------------
// Fixed-width font for testing and the 8x8 bitmap font renderer
// ---------------------------------------------------------------------------

struct FixedFont : IFont {
  uint16_t glyph_width;
  uint16_t line_height;

  explicit FixedFont(uint16_t gw = 8, uint16_t lh = 16) : glyph_width(gw), line_height(lh) {}

  uint16_t char_width(char32_t, FontStyle, uint8_t size_pct = 100) const override {
    return scale_width(size_pct);
  }

  uint16_t word_width(const char* text, size_t len, FontStyle, uint8_t size_pct = 100) const override {
    // Count Unicode codepoints, not bytes
    uint16_t count = 0;
    for (size_t i = 0; i < len;) {
      uint8_t b = static_cast<uint8_t>(text[i]);
      if (b < 0x80)
        i += 1;
      else if (b < 0xE0)
        i += 2;
      else if (b < 0xF0)
        i += 3;
      else
        i += 4;
      ++count;
    }
    return count * scale_width(size_pct);
  }

  uint16_t y_advance(uint8_t size_pct = 100) const override {
    return scale_height(size_pct);
  }

  uint16_t baseline(uint8_t size_pct = 100) const override {
    // Baseline at ~80% of line height (above descenders/leading)
    return scale_height(size_pct) * 4 / 5;
  }

 private:
  uint16_t scale_width(uint8_t /*size_pct*/) const {
    // Width is always fixed: the 8×8 bitmap font renders at exactly
    // glyph_width pixels per codepoint regardless of FontSize.
    // Returning different values for Small/Large causes layout positions
    // to diverge from draw_glyphs_(), producing overlapping text.
    return glyph_width;
  }
  uint16_t scale_height(uint8_t size_pct) const {
    return static_cast<uint16_t>(line_height * (size_pct / 100.0f));
  }
};

}  // namespace microreader
