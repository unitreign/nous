#pragma once

#include <cstdlib>

#include "../../Input.h"
#include "../../display/DrawBuffer.h"
#include "../IScreen.h"

namespace microreader {

class BouncingBallDemo final : public IScreen {
 public:
  BouncingBallDemo() = default;

  const char* name() const override {
    return "Bouncing Ball";
  }

  void start(DrawBuffer& buf, IRuntime& runtime) override;
  void stop() override;
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 private:
  static constexpr int kBallRadius = 30;

  int ball_cx_ = kBallRadius;
  int ball_cy_ = kBallRadius;
  int demo_vx_ = 5;
  int demo_vy_ = 5;

  struct RandRect {
    int x, y, w, h, countdown;
  };
  static constexpr int kNumRects = 10;
  static constexpr int kRectSizes[kNumRects][2] = {
      {80,  80 },
      {60,  60 },
      {100, 100},
      {50,  50 },
      {70,  40 },
      {90,  90 },
      {40,  70 },
      {55,  55 },
      {120, 45 },
      {45,  120},
  };
  RandRect rand_rects_[kNumRects] = {};

  struct RandCircle {
    int cx, cy, r, countdown;
  };
  static constexpr int kNumCircles = 5;
  static constexpr int kCircleRadii[kNumCircles] = {40, 25, 55, 35, 50};
  RandCircle rand_circles_[kNumCircles] = {};

  struct RandText {
    int x, y, countdown;
    const char* text;
  };
  static constexpr int kNumTexts = 5;
  static constexpr const char* kTextLabels[kNumTexts] = {"Hello", "World", "ePaper", "micro", "reader"};
  RandText rand_texts_[kNumTexts] = {};

  void draw_all_(DrawBuffer& buf) const;
};

}  // namespace microreader
