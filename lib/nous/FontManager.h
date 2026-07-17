#pragma once

#include "content/BitmapFont.h"
#include "display/DrawBuffer.h"

namespace microreader {

// Owns the BitmapFont slots and BitmapFontSet for one platform.
// Desktop uses this directly (ensure_ready is a no-op).
// ESP32 subclasses it to override ensure_ready with partition provisioning.
class FontManager {
 public:
  // Init one font slot. The data pointer must outlive this object.
  void load_font(const uint8_t* data, size_t sz) {
    if (num_fonts_ >= kMaxFontSizes)
      return;
    int idx = num_fonts_++;
    prop_fonts_[idx].init(data, sz);
    font_set_.add(&prop_fonts_[idx]);
  }

  // Load a combined FNTS bundle. The data pointer must outlive this object.
  bool load_bundle(const uint8_t* d, size_t sz) {
    if (sz < 40 || d[0] != 'F' || d[1] != 'N' || d[2] != 'T' || d[3] != 'S' || d[5] < 1) {
      return false;
    }

    uint8_t num = d[4];
    if (num > kMaxFontSizes)
      num = kMaxFontSizes;

    constexpr size_t kSizeTableOff = 8 + 32;
    uint32_t sizes[kMaxFontSizes] = {};
    for (int i = 0; i < num; i++) {
      const uint8_t* p = d + kSizeTableOff + i * 4;
      sizes[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    }

    size_t off = kSizeTableOff + static_cast<size_t>(num) * 4;
    for (int i = 0; i < num; i++) {
      if (off + sizes[i] > sz)
        break;
      load_font(d + off, sizes[i]);
      off += sizes[i];
    }
    return true;
  }

  // Return the font set (use with app.set_reader_font()).
  BitmapFontSet* font_set() {
    return font_set_.valid() ? &font_set_ : nullptr;
  }

  bool valid() const {
    return font_set_.valid();
  }

  // No provisioning needed on desktop.
  virtual void ensure_ready(DrawBuffer&) {}

 protected:
  BitmapFont prop_fonts_[kMaxFontSizes];
  BitmapFontSet font_set_;
  int num_fonts_ = 0;
};

}  // namespace microreader
