#pragma once

#include <SDL.h>

#include <cstring>
#include <vector>

#include "nous/display/DrawBuffer.h"
#include "runtime.h"

// Desktop e-ink emulator. Renders the pixel buffer to an SDL window.
// Maintains a float sim_ buffer to reproduce e-ink colour tonality;
// partial_refresh snaps only changed pixels so the paper texture is preserved.
class DesktopEmulatorDisplay final : public microreader::IDisplay {
 public:
  // Desktop renders the visible area only (788px wide), skipping the 12 hidden panel columns.
  static constexpr int kPixels = microreader::DisplayFrame::kPhysicalWidth * microreader::DisplayFrame::kPhysicalHeight;

  static constexpr uint32_t kRefreshDelayMs = 400;
  static constexpr uint32_t kRefreshDelayRegionMs = 10;  // used for the loading bar

  explicit DesktopEmulatorDisplay(DesktopRuntime& rt) : rt_(rt), sim_(kPixels, 1.0f) {}

  void set_rotation(microreader::Rotation r) override {
    rotation_ = r;
    rt_.apply_rotation(r);
  }

  void full_refresh(const uint8_t* pixels, microreader::RefreshMode /*mode*/, bool /*turnOffScreen*/) override {
    in_grayscale_mode_ = false;
    pre_gray_sim_.clear();
    for (int i = 0; i < kPixels; ++i) {
      const int y = i / microreader::DisplayFrame::kPhysicalWidth;
      const int x = i % microreader::DisplayFrame::kPhysicalWidth;
      const int x_buf = x + microreader::DisplayFrame::kPanelOffsetX;
      const std::size_t byte_idx = static_cast<std::size_t>(y * microreader::DisplayFrame::kStride + x_buf / 8);
      const uint8_t bit = static_cast<uint8_t>(0x80u >> (x_buf & 7));
      sim_[i] = (pixels[byte_idx] & bit) ? 1.0f : 0.0f;
    }
    render_();
    SDL_Delay(kRefreshDelayMs);
  }

  void partial_refresh(const uint8_t* new_pixels, const uint8_t* /*prev_pixels*/) override {
    if (in_grayscale_mode_)
      grayscale_revert_sim_();
    for (int y = 0; y < microreader::DisplayFrame::kPhysicalHeight; ++y) {
      for (int x = 0; x < microreader::DisplayFrame::kPhysicalWidth; ++x) {
        const int x_buf = x + microreader::DisplayFrame::kPanelOffsetX;
        const std::size_t byte_idx = static_cast<std::size_t>(y * microreader::DisplayFrame::kStride + x_buf / 8);
        const uint8_t bit = static_cast<uint8_t>(0x80u >> (x_buf & 7));
        const bool new_white = (new_pixels[byte_idx] & bit) != 0;
        const bool old_white = sim_[y * microreader::DisplayFrame::kPhysicalWidth + x] >= 0.5f;
        if (old_white != new_white)
          sim_[y * microreader::DisplayFrame::kPhysicalWidth + x] = new_white ? 1.0f : 0.0f;
        // Unchanged pixels keep their current sim_ value (preserves ghosting appearance).
      }
    }
    render_();
    SDL_Delay(kRefreshDelayMs);
  }

  // Store BW RAM data for subsequent grayscale_refresh.
  void write_ram_bw(const uint8_t* data) override {
    if (gray_bw_.empty())
      gray_bw_.resize(microreader::DisplayFrame::kPixelBytes);
    std::memcpy(gray_bw_.data(), data, microreader::DisplayFrame::kPixelBytes);
  }

  // Store RED RAM data for subsequent grayscale_refresh.
  void write_ram_red(const uint8_t* data) override {
    if (gray_red_.empty())
      gray_red_.resize(microreader::DisplayFrame::kPixelBytes);
    std::memcpy(gray_red_.data(), data, microreader::DisplayFrame::kPixelBytes);
  }

  void revert_grayscale(const uint8_t* /*prev_pixels*/) override {
    if (in_grayscale_mode_)
      grayscale_revert_sim_();
  }

  // Multi-pass anti-aliasing grayscale (kLutGrayscale encoding).
  // bit=0 in both planes = paper white (unchanged); any set bit = gray shade.
  void grayscale_refresh(bool /*turnOffScreen*/ = false) override {
    if (gray_bw_.empty() || gray_red_.empty())
      return;
    pre_gray_sim_ = sim_;
    in_grayscale_mode_ = true;
    for (int i = 0; i < kPixels; ++i) {
      const int y = i / microreader::DisplayFrame::kPhysicalWidth;
      const int x = i % microreader::DisplayFrame::kPhysicalWidth;
      const int x_buf = x + microreader::DisplayFrame::kPanelOffsetX;
      const std::size_t byte_idx = static_cast<std::size_t>(y * microreader::DisplayFrame::kStride + x_buf / 8);
      const uint8_t bit_mask = static_cast<uint8_t>(0x80u >> (x_buf & 7));
      const bool lsb_bit = (gray_bw_[byte_idx]  & bit_mask) != 0;
      const bool msb_bit = (gray_red_[byte_idx] & bit_mask) != 0;
      if (lsb_bit || msb_bit) {
        if (msb_bit && lsb_bit)  sim_[i] = 0.35f;
        else if (msb_bit)        sim_[i] = 0.50f;
        else                     sim_[i] = 0.70f;
      }
    }
    render_();
  }

  // One-pass sleep image grayscale (kLutFactoryQuality encoding).
  // state = (red_bit << 1) | bw_bit  â†’  0=black, 1=dark gray, 2=light gray, 3=white.
  void grayscale_refresh_1pass(bool /*turnOffScreen*/ = false) override {
    if (gray_bw_.empty() || gray_red_.empty())
      return;
    pre_gray_sim_ = sim_;
    in_grayscale_mode_ = true;
    static constexpr float kLevels[4] = {1.0f, 0.67f, 0.33f, 0.0f};
    for (int i = 0; i < kPixels; ++i) {
      const int y = i / microreader::DisplayFrame::kPhysicalWidth;
      const int x = i % microreader::DisplayFrame::kPhysicalWidth;
      const int x_buf = x + microreader::DisplayFrame::kPanelOffsetX;
      const std::size_t byte_idx = static_cast<std::size_t>(y * microreader::DisplayFrame::kStride + x_buf / 8);
      const uint8_t bit_mask = static_cast<uint8_t>(0x80u >> (x_buf & 7));
      const int bw_bit  = (gray_bw_[byte_idx]  & bit_mask) ? 1 : 0;
      const int red_bit = (gray_red_[byte_idx] & bit_mask) ? 1 : 0;
      sim_[i] = kLevels[(red_bit << 1) | bw_bit];
    }
    render_();
  }

  bool in_grayscale_mode() const override {
    return in_grayscale_mode_;
  }

  void partial_refresh_region(int phys_x, int phys_y, int phys_w, int phys_h, const uint8_t* new_buf,
                              int stride_bytes) override {
    // phys_x is raw hardware column; convert to sim_ app-space index by subtracting panel offset.
    const int sim_x0 = phys_x - microreader::DisplayFrame::kPanelOffsetX;
    for (int row = 0; row < phys_h; ++row) {
      const int y = phys_y + row;
      if (y < 0 || y >= microreader::DisplayFrame::kPhysicalHeight)
        continue;
      const uint8_t* src = new_buf + row * stride_bytes;
      for (int col = 0; col < phys_w; ++col) {
        const int x = sim_x0 + col;
        if (x < 0 || x >= microreader::DisplayFrame::kPhysicalWidth)
          continue;
        const bool white = (src[col / 8] >> (7 - (col & 7))) & 1;
        sim_[y * microreader::DisplayFrame::kPhysicalWidth + x] = white ? 1.0f : 0.0f;
      }
    }
    render_();
    SDL_Delay(kRefreshDelayRegionMs);
  }

 private:
  DesktopRuntime& rt_;
  microreader::Rotation rotation_ = microreader::Rotation::Deg0;
  std::vector<float> sim_;
  std::vector<float> pre_gray_sim_;  // sim_ snapshot before grayscale overlay
  bool in_grayscale_mode_ = false;
  std::vector<uint8_t> gray_bw_;   // BW RAM (state bit 0) staged for grayscale_refresh
  std::vector<uint8_t> gray_red_;  // RED RAM (state bit 1) staged for grayscale_refresh

  // Simulate grayscale revert: restore the pre-grayscale BW state.
  void grayscale_revert_sim_() {
    in_grayscale_mode_ = false;
    if (!pre_gray_sim_.empty()) {
      sim_ = pre_gray_sim_;
      pre_gray_sim_.clear();
      render_();
    }
  }

  // E-ink palette: RGB endpoints for black (s=0) and white (s=1).
  static constexpr uint8_t kBlackR = 0x18, kBlackG = 0x1A, kBlackB = 0x1C;
  static constexpr uint8_t kWhiteR = 0xE8, kWhiteG = 0xDC, kWhiteB = 0xC8;

  void render_() {
    void* raw = nullptr;
    int pitch = 0;
    SDL_LockTexture(rt_.texture(), nullptr, &raw, &pitch);
    auto* p = static_cast<uint8_t*>(raw);
    for (int y = 0; y < microreader::DisplayFrame::kPhysicalHeight; ++y) {
      uint8_t* row = p + y * pitch;
      for (int x = 0; x < microreader::DisplayFrame::kPhysicalWidth; ++x) {
        const float s = sim_[y * microreader::DisplayFrame::kPhysicalWidth + x];
        row[x * 3 + 0] = static_cast<uint8_t>(kBlackR + s * (kWhiteR - kBlackR));
        row[x * 3 + 1] = static_cast<uint8_t>(kBlackG + s * (kWhiteG - kBlackG));
        row[x * 3 + 2] = static_cast<uint8_t>(kBlackB + s * (kWhiteB - kBlackB));
      }
    }
    SDL_UnlockTexture(rt_.texture());

    const bool sideways = rotation_ == microreader::Rotation::Deg90;
    const int win_w = sideways ? microreader::DisplayFrame::kPhysicalHeight : microreader::DisplayFrame::kPhysicalWidth;
    const int win_h = sideways ? microreader::DisplayFrame::kPhysicalWidth : microreader::DisplayFrame::kPhysicalHeight;
    SDL_Rect dst = {(win_w - microreader::DisplayFrame::kPhysicalWidth) / 2,
                    (win_h - microreader::DisplayFrame::kPhysicalHeight) / 2, microreader::DisplayFrame::kPhysicalWidth,
                    microreader::DisplayFrame::kPhysicalHeight};
    SDL_RenderClear(rt_.renderer());
    SDL_RenderCopyEx(rt_.renderer(), rt_.texture(), nullptr, &dst, static_cast<double>(static_cast<int>(rotation_)),
                     nullptr, SDL_FLIP_NONE);
    SDL_RenderPresent(rt_.renderer());
  }
};
