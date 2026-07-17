#pragma once

#include "../../Input.h"
#include "../../display/DrawBuffer.h"
#include "../IScreen.h"

namespace microreader {

// Demo screen rendering the test_image_grayscale.h image with 5-level grayscale.
// Button2 (down): rotate image 90° CW.
// Button3 (up):   flip image horizontally.
// Up:             full refresh.
// Button1:        toggle grayscale on/off.
// Button0:        exit.
class GrayscaleDemo final : public IScreen {
 public:
  GrayscaleDemo() = default;

  const char* name() const override {
    return "Grayscale Demo";
  }

  void start(DrawBuffer& buf, IRuntime& runtime) override;
  void stop() override;
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 private:
  int rotation_ = 0;  // 0..3 — multiples of 90° CW
  bool flip_h_ = false;

  // Draw a packed 1-bit plane into the inactive buffer applying current transform.
  // src_white_bit: value of a source bit that means "white" (1 for BW plane since
  //   BW pixels are black, 0 for LSB/MSB planes where 0 means "not set").
  // fill_white: initial fill colour for the inactive buffer before drawing.
  void draw_plane_(DrawBuffer& buf, const uint8_t* data, int src_w, int src_h, bool fill_white,
                   bool black_bit_is_one) const;

  void draw_bw_(DrawBuffer& buf) const;
  void apply_grayscale_(DrawBuffer& buf) const;
};

}  // namespace microreader
