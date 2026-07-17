#pragma once

#include <SDL.h>

#include <stdexcept>
#include <string>

#include "nous/Input.h"
#include "nous/Runtime.h"
#include "nous/display/DrawBuffer.h"

// Window scale factor: each display pixel becomes kScaleÃ—kScale screen pixels.
static constexpr int kDisplayScale = 1;

class DesktopRuntime final : public microreader::IRuntime {
 public:
  explicit DesktopRuntime(uint32_t frame_time_ms) : frame_time_ms_(frame_time_ms) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
      throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    }
    window_ = SDL_CreateWindow("microreader", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               microreader::DisplayFrame::kPhysicalWidth * kDisplayScale,
                               microreader::DisplayFrame::kPhysicalHeight * kDisplayScale, SDL_WINDOW_SHOWN);
    if (!window_)
      throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_)
      throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());

    // Nearest-neighbour scaling so individual pixels stay crisp when zoomed.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(renderer_, microreader::DisplayFrame::kPhysicalWidth,
                             microreader::DisplayFrame::kPhysicalHeight);

    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                 microreader::DisplayFrame::kPhysicalWidth, microreader::DisplayFrame::kPhysicalHeight);
    if (!texture_)
      throw std::runtime_error(std::string("SDL_CreateTexture: ") + SDL_GetError());
  }

  ~DesktopRuntime() {
    if (texture_)
      SDL_DestroyTexture(texture_);
    if (renderer_)
      SDL_DestroyRenderer(renderer_);
    if (window_)
      SDL_DestroyWindow(window_);
    SDL_Quit();
  }

  SDL_Renderer* renderer() const {
    return renderer_;
  }
  SDL_Texture* texture() const {
    return texture_;
  }

  // Resize the window and update the logical render size to match the rotation.
  // 0Â°/180Â° â†’ 800Ã—480 window; 90Â°/270Â° â†’ 480Ã—800 window.
  void apply_rotation(microreader::Rotation rotation) {
    const bool sideways = rotation == microreader::Rotation::Deg90;
    const int win_w =
        (sideways ? microreader::DisplayFrame::kPhysicalHeight : microreader::DisplayFrame::kPhysicalWidth) *
        kDisplayScale;
    const int win_h =
        (sideways ? microreader::DisplayFrame::kPhysicalWidth : microreader::DisplayFrame::kPhysicalHeight) *
        kDisplayScale;
    SDL_SetWindowSize(window_, win_w, win_h);
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_RenderSetLogicalSize(
        renderer_, sideways ? microreader::DisplayFrame::kPhysicalHeight : microreader::DisplayFrame::kPhysicalWidth,
        sideways ? microreader::DisplayFrame::kPhysicalWidth : microreader::DisplayFrame::kPhysicalHeight);
  }

  // Register a bool to flip when the user presses T.
  void set_transition_toggle(bool* flag) {
    transition_flag_ = flag;
  }

  std::optional<uint8_t> battery_percentage() const override {
    return 50;
  }

  // IRuntime: step mode (P toggles, Space advances one tick).
  bool step_mode() const override {
    return step_mode_;
  }
  bool consume_step() override {
    if (!step_requested_)
      return false;
    step_requested_ = false;
    return true;
  }

  // IRuntime
  bool should_continue() const override {
    return !quit_;
  }

  // Consume all queued press events accumulated since the last call.
  // Fills history[] (up to kMaxPressHistory entries) and derives pressed_latch.
  void consume_press_events(uint8_t* history, uint8_t& count, uint8_t& latch) {
    count = 0;
    latch = 0;
    while (pq_head_ != pq_tail_) {
      const uint8_t btn_idx = press_queue_[pq_head_];
      pq_head_ = static_cast<uint8_t>((pq_head_ + 1) % kQueueSize);
      latch |= static_cast<uint8_t>(1u << btn_idx);
      if (count < microreader::ButtonState::kMaxPressHistory)
        history[count++] = btn_idx;
    }
  }

  // Process SDL window/debug events. Call once per frame before polling input.
  void pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT)
        quit_ = true;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_t && transition_flag_)
        *transition_flag_ = !*transition_flag_;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_p)
        step_mode_ = !step_mode_;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_SPACE)
        step_requested_ = true;

      // Enqueue button key-down events so brief presses between frames are not lost.
      if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        using B = microreader::Button;
        auto enqueue = [&](B btn) {
          const uint8_t i = static_cast<uint8_t>(btn);
          const uint8_t next_tail = static_cast<uint8_t>((pq_tail_ + 1) % kQueueSize);
          if (next_tail != pq_head_) {
            press_queue_[pq_tail_] = i;
            pq_tail_ = next_tail;
          }
        };
        switch (e.key.keysym.scancode) {
          case SDL_SCANCODE_LEFT:
            enqueue(B::Button0);
            break;
          case SDL_SCANCODE_RIGHT:
            enqueue(B::Button1);
            break;
          case SDL_SCANCODE_DOWN:
            enqueue(B::Button2);
            break;
          case SDL_SCANCODE_UP:
            enqueue(B::Button3);
            break;
          case SDL_SCANCODE_Q:
            enqueue(B::Up);
            break;
          case SDL_SCANCODE_A:
            enqueue(B::Down);
            break;
          case SDL_SCANCODE_RETURN:
            enqueue(B::Power);
            break;
          default:
            break;
        }
      }
    }

    // (auto-repeat removed â€” screens handle hold-down acceleration themselves)
  }

  uint32_t frame_time_ms() const override {
    return frame_time_ms_;
  }

  void wait_next_frame() override {
    SDL_Delay(frame_time_ms_);
  }

 private:
  static constexpr uint8_t kNumButtons = microreader::ButtonState::kButtonCount;
  static constexpr uint8_t kQueueSize = 32;

  uint32_t frame_time_ms_;
  bool quit_ = false;
  bool* transition_flag_ = nullptr;
  uint8_t press_queue_[kQueueSize] = {};
  uint8_t pq_head_ = 0;
  uint8_t pq_tail_ = 0;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  bool step_mode_ = false;
  bool step_requested_ = false;
};
