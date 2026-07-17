#include "GrayscaleDemo.h"

#include <cstring>

#include "../../../../resources/test_image_grayscale.h"
#include "../../Application.h"

namespace microreader {

// Draw a transformed 1-bit packed plane into the inactive buffer.
// In the packed format each bit represents one pixel; the bit is 1 where the
// plane is "active" (black in BW, set in LSB/MSB).
//   fill_white         – initial fill colour before drawing
//   active_bit_is_dark – if true, an active (1) bit draws a dark (false) pixel;
//                        otherwise an active (1) bit draws a light (true) pixel.
void GrayscaleDemo::draw_plane_(DrawBuffer& buf, const uint8_t* data, int src_w, int src_h, bool fill_white,
                                bool active_bit_is_dark) const {
  buf.fill(fill_white);

  // Displayed dimensions after rotation.
  const int disp_w = (rotation_ & 1) ? src_h : src_w;
  const int disp_h = (rotation_ & 1) ? src_w : src_h;
  const int ox = (DrawBuffer::kWidth - disp_w) / 2;
  const int oy = (DrawBuffer::kHeight - disp_h) / 2;

  const int src_stride = (src_w + 7) / 8;

  for (int sy = 0; sy < src_h; ++sy) {
    const uint8_t* row = data + sy * src_stride;
    for (int sx = 0; sx < src_w; ++sx) {
      const bool active = (row[sx >> 3] >> (7 - (sx & 7))) & 1;
      if (!active)
        continue;  // skip inactive pixels; background already filled

      // Apply horizontal flip.
      const int fx = flip_h_ ? (src_w - 1 - sx) : sx;
      const int fy = sy;

      // Apply rotation (CW).
      int lx, ly;
      switch (rotation_) {
        case 1:
          lx = src_h - 1 - fy;
          ly = fx;
          break;
        case 2:
          lx = src_w - 1 - fx;
          ly = src_h - 1 - fy;
          break;
        case 3:
          lx = fy;
          ly = src_w - 1 - fx;
          break;
        default:
          lx = fx;
          ly = fy;
          break;
      }

      buf.set_pixel(lx + ox, ly + oy, !active_bit_is_dark);
    }
  }
}

void GrayscaleDemo::draw_bw_(DrawBuffer& buf) const {
  // BW pass: white background, draw dark pixels as black.
  draw_plane_(buf, test_image_bw, test_image_width, test_image_height,
              /*fill_white=*/true, /*active_bit_is_dark=*/true);
  buf.draw_text(4, 4, "Btn2:Rotate Btn3:Flip Up:Full Btn1:Gray", true, 1);
}

void GrayscaleDemo::apply_grayscale_(DrawBuffer& buf) const {
  // LSB plane → BW RAM (active bit = 1 in BW RAM = white pixel in buffer)
  draw_plane_(buf, test_image_lsb, test_image_width, test_image_height,
              /*fill_white=*/false, /*active_bit_is_dark=*/false);
  buf.write_ram_bw();

  // MSB plane → RED RAM
  draw_plane_(buf, test_image_msb, test_image_width, test_image_height,
              /*fill_white=*/false, /*active_bit_is_dark=*/false);
  buf.write_ram_red();

  buf.grayscale_refresh();
}

void GrayscaleDemo::start(DrawBuffer& buf, IRuntime& runtime) {
  rotation_ = 0;
  flip_h_ = false;
  draw_bw_(buf);
}

void GrayscaleDemo::stop() {}

void GrayscaleDemo::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& /*runtime*/) {
  if (buttons.is_pressed(Button::Button0)) {
    app_->pop_screen();
    return;
  }

  bool is_gray = buf.display().in_grayscale_mode();

  if (buttons.is_pressed(Button::Button1)) {
    if (is_gray)
      buf.revert_grayscale();
    else
      apply_grayscale_(buf);
    return;
  }

  bool changed = false;
  if (buttons.is_pressed(Button::Button2)) {
    rotation_ = (rotation_ + 1) & 3;
    changed = true;
  }
  if (buttons.is_pressed(Button::Button3)) {
    flip_h_ = !flip_h_;
    changed = true;
  }

  if (buttons.is_pressed(Button::Up)) {
    draw_bw_(buf);
    buf.full_refresh();
  } else if (changed) {
    draw_bw_(buf);
    buf.refresh();  // partial BW refresh (auto-reverts grayscale if active)
  }
}

}  // namespace microreader
