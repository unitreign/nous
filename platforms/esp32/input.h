#pragma once

#include <cstdint>
#include <cstdlib>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nous/Input.h"

// Defined in serial_communication.h â€” serial command button injection.
extern volatile uint8_t g_serial_buttons;

// ---- Button hardware configuration ----
//
// ADC1 channel 1 (GPIO1): 4 buttons via resistor ladder
//   index 0 â†’ Button0 (Back),    threshold â‰ˆ 3470
//   index 1 â†’ Button1 (Confirm), threshold â‰ˆ 2655
//   index 2 â†’ Button2 (Left),    threshold â‰ˆ 1470
//   index 3 â†’ Button3 (Right),   threshold â‰ˆ 3
//
// ADC1 channel 2 (GPIO2): 2 buttons via resistor ladder
//   index 0 â†’ Up   (Vol+), threshold â‰ˆ 2205
//   index 1 â†’ Down (Vol-), threshold â‰ˆ 3
//
// GPIO 3: Power button, digital active-LOW with internal pull-up
//
// Buttons are sampled every 5ms by an esp_timer callback. Rising edges
// are latched so that brief presses between frame polls are never lost.

class Esp32InputSource final : public microreader::IInputSource {
 public:
  Esp32InputSource() {
    // Configure power button GPIO with internal pull-up
    gpio_config_t cfg{};
    cfg.pin_bit_mask = (1ULL << kPowerPin);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    // Initialise ADC1 in oneshot mode, 12 dB attenuation (full 0â€“3.3 V range)
#ifndef QEMU_BUILD
    adc_oneshot_unit_init_cfg_t unit_cfg{};
    unit_cfg.unit_id = ADC_UNIT_1;
    adc_oneshot_new_unit(&unit_cfg, &adc_handle_);

    adc_oneshot_chan_cfg_t ch_cfg{};
    ch_cfg.atten = ADC_ATTEN_DB_12;
    ch_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_1, &ch_cfg);  // GPIO1
    adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_2, &ch_cfg);  // GPIO2

    // Start periodic timer for continuous button sampling
    esp_timer_create_args_t timer_args{};
    timer_args.callback = sample_timer_cb;
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    esp_timer_create(&timer_args, &sample_timer_);
    esp_timer_start_periodic(sample_timer_, kSampleIntervalUs);
#endif  // !QEMU_BUILD
  }

  // Returns accumulated button press history since last poll (in arrival order),
  // then clears the queue. pressed_latch is also derived for backward compat.
  microreader::ButtonState poll_buttons() override {
    microreader::ButtonState result;
    portENTER_CRITICAL(&lock_);
    result.current = debounced_;
    // Drain the ring buffer into the press history.
    while (pq_head_ != pq_tail_) {
      const uint8_t btn_idx = press_queue_[pq_head_];
      pq_head_ = static_cast<uint8_t>((pq_head_ + 1) % kQueueSize);
      result.pressed_latch |= static_cast<uint8_t>(1u << btn_idx);
      if (result.press_history_count < microreader::ButtonState::kMaxPressHistory)
        result.press_history[result.press_history_count++] = btn_idx;
    }
    // Merge any button presses injected via serial commands.
    uint8_t serial = g_serial_buttons;
    if (serial) {
      for (uint8_t i = 0; i < kNumButtons; ++i) {
        if (serial & static_cast<uint8_t>(1u << i)) {
          result.pressed_latch |= static_cast<uint8_t>(1u << i);
          if (result.press_history_count < microreader::ButtonState::kMaxPressHistory)
            result.press_history[result.press_history_count++] = i;
        }
      }
      g_serial_buttons = 0;
    }
    portEXIT_CRITICAL(&lock_);
    return result;
  }

  // Remove all queued events for a single button without affecting others.
  void clear_button(microreader::Button button) {
    const uint8_t btn_idx = static_cast<uint8_t>(button);
    portENTER_CRITICAL(&lock_);
    // Compact the ring buffer, dropping entries for this button.
    uint8_t new_head = pq_head_;
    uint8_t new_tail = pq_head_;
    uint8_t r = pq_head_;
    while (r != pq_tail_) {
      if (press_queue_[r] != btn_idx) {
        press_queue_[new_tail] = press_queue_[r];
        new_tail = static_cast<uint8_t>((new_tail + 1) % kQueueSize);
      }
      r = static_cast<uint8_t>((r + 1) % kQueueSize);
    }
    pq_head_ = new_head;
    pq_tail_ = new_tail;
    portEXIT_CRITICAL(&lock_);
  }

 public:
  adc_oneshot_unit_handle_t get_adc_handle() const {
    return adc_handle_;
  }

 private:
  static constexpr gpio_num_t kPowerPin = GPIO_NUM_3;
  static constexpr int kAdcTol = 400;     // Â±tolerance for threshold match
  static constexpr int kAdcNoBtn = 3800;  // ADC value when no key pressed
  static constexpr uint8_t kNumButtons = 7;
  static constexpr uint32_t kDebounceMs = 5;
  static constexpr uint64_t kSampleIntervalUs = 10000;  // 10 ms (100 Hz)

  static constexpr int kThresh1[4] = {3470, 2655, 1470, 3};  // Button0â€“3
  static constexpr int kThresh2[2] = {2205, 3};              // Up, Down

  adc_oneshot_unit_handle_t adc_handle_ = nullptr;
  esp_timer_handle_t sample_timer_ = nullptr;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;

  // Debounce state â€” only modified by the timer callback
  uint8_t debounced_ = 0;
  uint8_t prev_debounced_ = 0;
  bool last_raw_[kNumButtons] = {};
  uint32_t debounce_time_[kNumButtons] = {};

  // (auto-repeat removed â€” screens handle hold-down acceleration themselves)

  // Ordered press queue â€” shared between timer callback (write) and poll_buttons (read+clear).
  // Ring buffer; entries are button indices (0â€“6). Full entries are silently dropped.
  static constexpr uint8_t kQueueSize = 32;
  uint8_t press_queue_[kQueueSize] = {};
  uint8_t pq_head_ = 0;  // next read position
  uint8_t pq_tail_ = 0;  // next write position

  static void sample_timer_cb(void* arg) {
    static_cast<Esp32InputSource*>(arg)->sample();
  }

  void sample() {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const uint8_t raw = read_raw();

    // Per-button debounce (both press and release).
    for (uint8_t i = 0; i < kNumButtons; ++i) {
      const uint8_t mask = static_cast<uint8_t>(1u << i);
      const bool raw_down = (raw & mask) != 0;
      const bool current_down = (debounced_ & mask) != 0;

      if (raw_down != last_raw_[i]) {
        last_raw_[i] = raw_down;
        debounce_time_[i] = now;
      }

      if (raw_down != current_down) {
        if ((now - debounce_time_[i]) >= kDebounceMs) {
          if (raw_down)
            debounced_ |= mask;
          else
            debounced_ &= static_cast<uint8_t>(~mask);
        }
      }
    }

    // Detect rising edges.
    const uint8_t rising = debounced_ & ~prev_debounced_;

    // Enqueue rising edges into the press ring buffer.
    if (rising) {
      portENTER_CRITICAL(&lock_);
      for (uint8_t i = 0; i < kNumButtons; ++i) {
        if (rising & static_cast<uint8_t>(1u << i)) {
          const uint8_t next_tail = static_cast<uint8_t>((pq_tail_ + 1) % kQueueSize);
          if (next_tail != pq_head_) {  // drop if full
            press_queue_[pq_tail_] = i;
            pq_tail_ = next_tail;
          }
        }
      }
      portEXIT_CRITICAL(&lock_);
    }

    prev_debounced_ = debounced_;
  }

  uint8_t read_raw() const {
    uint8_t state = 0;

    // ADC1_CH1 (GPIO1) â†’ Button0..Button3 (bits 0â€“3)
    int adc1 = 0;
    adc_oneshot_read(adc_handle_, ADC_CHANNEL_1, &adc1);
    if (adc1 <= kAdcNoBtn) {
      for (int i = 0; i < 4; ++i) {
        if (abs(adc1 - kThresh1[i]) < kAdcTol) {
          state |= static_cast<uint8_t>(1u << i);
          break;
        }
      }
    }

    // ADC1_CH2 (GPIO2) â†’ Up (bit 4), Down (bit 5)
    int adc2 = 0;
    adc_oneshot_read(adc_handle_, ADC_CHANNEL_2, &adc2);
    if (adc2 <= kAdcNoBtn) {
      for (int i = 0; i < 2; ++i) {
        if (abs(adc2 - kThresh2[i]) < kAdcTol) {
          state |= static_cast<uint8_t>(1u << (i + 4));
          break;
        }
      }
    }

    // GPIO3, active LOW â†’ Power (bit 6)
    if (gpio_get_level(kPowerPin) == 0)
      state |= static_cast<uint8_t>(1u << 6);

    return state;
  }
};
