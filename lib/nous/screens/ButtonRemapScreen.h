#pragma once

#include "../Input.h"
#include "../content/BitmapFont.h"
#include "../display/DrawBuffer.h"
#include "IScreen.h"

namespace microreader {

class ButtonRemapScreen final : public IScreen {
 public:
  ButtonRemapScreen() = default;
  const char* name() const override { return "Button Remap"; }
  void start(DrawBuffer& buf, IRuntime& runtime) override;
  void stop() override {}
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 private:
  int step_ = 0;          // 0 = press NEXT PAGE button, 1 = side layout picker
  uint8_t pending_front_ = 2;  // 2=Left, 3=Right
  int side_sel_ = 1;      // 0=Off, 1=UpNext, 2=DownNext

  BitmapFont title_font_;
  BitmapFont body_font_;
  BitmapFont hint_font_;

  void load_fonts_();
  void draw_(DrawBuffer& buf);
};

}  // namespace microreader
