#include "BouncingBallDemo.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "../../Application.h"

namespace microreader {

void BouncingBallDemo::draw_all_(DrawBuffer& buf) const {
  buf.fill(true);
  for (const auto& r : rand_rects_)
    buf.fill_rect(r.x, r.y, r.w, r.h, false);
  for (const auto& c : rand_circles_)
    buf.draw_circle(c.cx, c.cy, c.r, false);
  buf.draw_circle(ball_cx_, ball_cy_, kBallRadius, false);
  for (const auto& t : rand_texts_)
    buf.draw_text(t.x, t.y, t.text, true, 1);
}

void BouncingBallDemo::start(DrawBuffer& buf, IRuntime& runtime) {
  const int W = DrawBuffer::kWidth;
  const int H = DrawBuffer::kHeight;

  ball_cx_ = kBallRadius;
  ball_cy_ = kBallRadius;

  for (int i = 0; i < kNumRects; ++i) {
    const int w = kRectSizes[i][0], h = kRectSizes[i][1];
    rand_rects_[i] = {std::rand() % std::max(1, W - w), std::rand() % std::max(1, H - h), w, h, 25 + std::rand() % 76};
  }
  for (int i = 0; i < kNumCircles; ++i) {
    const int r = kCircleRadii[i];
    rand_circles_[i] = {r + std::rand() % std::max(1, W - 2 * r), r + std::rand() % std::max(1, H - 2 * r), r,
                        25 + std::rand() % 76};
  }
  for (int i = 0; i < kNumTexts; ++i) {
    const int tw = static_cast<int>(std::strlen(kTextLabels[i])) * 8;
    rand_texts_[i] = {std::rand() % std::max(1, W - tw), std::rand() % std::max(1, H - 8), 25 + std::rand() % 76,
                      kTextLabels[i]};
  }

  draw_all_(buf);
}

void BouncingBallDemo::stop() {}

void BouncingBallDemo::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& /*runtime*/) {
  if (buttons.is_pressed(Button::Button0)) {
    app_->pop_screen();
    return;
  }

  const int W = DrawBuffer::kWidth;
  const int H = DrawBuffer::kHeight;

  ball_cx_ += demo_vx_;
  ball_cy_ += demo_vy_;
  if (ball_cx_ <= kBallRadius) {
    ball_cx_ = kBallRadius;
    demo_vx_ = -demo_vx_;
  }
  if (ball_cx_ >= W - kBallRadius - 1) {
    ball_cx_ = W - kBallRadius - 1;
    demo_vx_ = -demo_vx_;
  }
  if (ball_cy_ <= kBallRadius) {
    ball_cy_ = kBallRadius;
    demo_vy_ = -demo_vy_;
  }
  if (ball_cy_ >= H - kBallRadius - 1) {
    ball_cy_ = H - kBallRadius - 1;
    demo_vy_ = -demo_vy_;
  }

  for (auto& r : rand_rects_) {
    if (--r.countdown <= 0) {
      r.x = std::rand() % std::max(1, W - r.w);
      r.y = std::rand() % std::max(1, H - r.h);
      r.countdown = 25 + std::rand() % 76;
    }
  }
  for (auto& c : rand_circles_) {
    if (--c.countdown <= 0) {
      c.cx = c.r + std::rand() % std::max(1, W - 2 * c.r);
      c.cy = c.r + std::rand() % std::max(1, H - 2 * c.r);
      c.countdown = 25 + std::rand() % 76;
    }
  }
  for (auto& t : rand_texts_) {
    if (--t.countdown <= 0) {
      const int tw = static_cast<int>(std::strlen(t.text)) * 8;
      t.x = std::rand() % std::max(1, W - tw);
      t.y = std::rand() % std::max(1, H - 8);
      t.countdown = 25 + std::rand() % 76;
    }
  }

  draw_all_(buf);
  buf.refresh();
}

}  // namespace microreader