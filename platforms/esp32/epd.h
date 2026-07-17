#pragma once

// EInkDisplay driver for ESP-IDF (SSD1677).

#include <cstdint>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nous/display/DrawBuffer.h"

// ---- Pin assignments ----
#define EPD_SCLK GPIO_NUM_8
#define EPD_MOSI GPIO_NUM_10
#define EPD_CS GPIO_NUM_21
#define EPD_DC GPIO_NUM_4
#define EPD_RST GPIO_NUM_5
#define EPD_BUSY GPIO_NUM_6

// ---- SSD1677 command definitions ----
#define CMD_SOFT_RESET 0x12
#define CMD_BOOSTER_SOFT_START 0x0C
#define CMD_DRIVER_OUTPUT_CONTROL 0x01
#define CMD_BORDER_WAVEFORM 0x3C
#define CMD_TEMP_SENSOR_CONTROL 0x18
#define CMD_DATA_ENTRY_MODE 0x11
#define CMD_SET_RAM_X_RANGE 0x44
#define CMD_SET_RAM_Y_RANGE 0x45
#define CMD_SET_RAM_X_COUNTER 0x4E
#define CMD_SET_RAM_Y_COUNTER 0x4F
#define CMD_WRITE_RAM_BW 0x24
#define CMD_WRITE_RAM_RED 0x26
#define CMD_DISPLAY_UPDATE_CTRL1 0x21
#define CMD_DISPLAY_UPDATE_CTRL2 0x22
#define CMD_MASTER_ACTIVATION 0x20
#define CTRL1_NORMAL 0x00
#define CTRL1_BYPASS_RED 0x40
#define CMD_GATE_VOLTAGE 0x03
#define CMD_SOURCE_VOLTAGE 0x04
#define CMD_WRITE_VCOM 0x2C
#define CMD_WRITE_LUT 0x32
#define CMD_AUTO_WRITE_BW_RAM 0x46
#define CMD_AUTO_WRITE_RED_RAM 0x47
#define CMD_DEEP_SLEEP 0x10
#define CMD_WRITE_TEMP 0x1A

// clang-format off
// ---- Grayscale LUT tables (112 bytes each) ----
// Layout: 50 bytes VS waveform (5 levels × 10), 50 bytes TP/RP timing (10 groups × 5),
//         5 bytes frame rate, 1 byte VGH, 3 bytes VSH1/VSH2/VSL, 1 byte VCOM, 2 reserved.
static const uint8_t kLutGrayscale[] = {
    // VS L0–L3 (voltage patterns per transition)
    // Black → Black: [VSS → VSS → VSS → VSS → VSS → VSS → VSS → VSS]
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // Black → White: [VSL → VSL → VSL → VSL → VSL → VSL → VSL → VSH1 → VSL → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSS → VSS]
    0xAA,0xA9,0x95,0x54,0x00,0x00,0x00,0x00,0x00,0x00,
    // White → Black: [VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSL → VSL → VSL → VSL → VSL → VSL → VSL → VSL → VSL → VSL → VSS]
    0x55,0x5A,0xAA,0xAA,0x00,0x00,0x00,0x00,0x00,0x00,
    // White → White: [VSH1 → VSH1 → VSH1 → VSH1 → VSL → VSL → VSL → VSL → VSL → VSL → VSS → VSS → VSS → VSS → VSS → VSS → VSS]
    0x55,0xAA,0xA0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // L4 VCOM: [VSS → VSS → VSS → VSS → VSS → VSS → VSS → VSS]
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

    // TP/RP groups
    0x01,0x01,0x01,0x01,0x00,  // G0: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01,0x01,0x01,0x01,0x00,  // G1: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01,0x01,0x01,0x01,0x00,  // G2: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01,0x01,0x01,0x01,0x00,  // G3: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01,0x00,0x00,0x00,0x00,  // G4: A=1 B=0 C=0 D=0 RP=0 (1 frames)
    0x01,0x00,0x00,0x00,0x00,  // G5: A=1 B=0 C=0 D=0 RP=0 (1 frames)
    0x00,0x00,0x00,0x00,0x00,  // G6: A=0 B=0 C=0 D=0 RP=0
    0x00,0x00,0x00,0x00,0x00,  // G7: A=0 B=0 C=0 D=0 RP=0
    0x00,0x00,0x00,0x00,0x00,  // G8: A=0 B=0 C=0 D=0 RP=0
    0x00,0x00,0x00,0x00,0x00,  // G9: A=0 B=0 C=0 D=0 RP=0

    // Frame rate
    0x88,0x88,0x88,0x88,0x88,

    // Voltages (VGH, VSH1, VSH2, VSL, VCOM)
    0x17,0x41,0x8E,0x3A,0x30,
    // Reserved
    0x00, 0x00,
};

static const uint8_t kLutGrayscaleRevert[] = {
    // VS L0–L3 (voltage patterns per transition)
    // Black → Black: [VSS]
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // Black → White: [VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1]
    0x55,0x55,0x55,0x55,0x55,0x55,0x40,0x00,0x00,0x00,
    // White → Black: [VSH1 → VSH1 → VSL → VSL → VSH1 → VSH1 → VSL → VSL → VSH1 → VSH1 → VSL → VSL → VSL → VSL → VSL → VSL → VSL → VSL → VSL → VSL → VSS]
    0x5A,0x5A,0x5A,0xAA,0xAA,0x00,0x00,0x00,0x00,0x00,
    // White → White: [VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1 → VSH1]
    0x55,0x55,0x55,0x55,0x55,0x55,0x40,0x00,0x00,0x00,
    // L4 VCOM: [VSS]
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

    // TP/RP groups
    0x01,0x01,0x01,0x01,0x00,  // G0: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01,0x01,0x01,0x01,0x00,  // G1: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01,0x01,0x01,0x01,0x00,  // G2: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01,0x01,0x01,0x01,0x00,  // G3: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01,0x01,0x01,0x01,0x00,  // G4: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01,0x01,0x01,0x01,0x00,  // G5: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01,0x00,0x00,0x00,0x00,  // G6: A=1 B=0 C=0 D=0 RP=0 (1 frames)
    0x00,0x00,0x00,0x00,0x00,  // G7: A=0 B=0 C=0 D=0 RP=0
    0x00,0x00,0x00,0x00,0x00,  // G8: A=0 B=0 C=0 D=0 RP=0
    0x00,0x00,0x00,0x00,0x00,  // G9: A=0 B=0 C=0 D=0 RP=0

    // Frame rate
    0x88,0x88,0x88,0x88,0x88,

    // Voltages (VGH, VSH1, VSH2, VSL, VCOM)
    0x17,0x4B,0x8E,0x3A,0x30,
    // Reserved
    0x00, 0x00,
};

// One-pass 4-level grayscale LUT.
// State bits are (RED_bit << 1 | BW_bit), mapping directly to 4 gray levels:
//   00 = white, 01 = light gray, 10 = dark gray, 11 = black
static const uint8_t kLutFactoryQuality[] = {
    // VS L0–L3 + VCOM (10 bytes each)
    0x00,0x4A,0x88,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // LUT0: state 00 (black)
    0x80,0x62,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // LUT1: state 01 (dark gray)
    0x88,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // LUT2: state 10 (light gray)
    0xA8,0x44,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // LUT3: state 11 (white)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // LUT4: VCOM

    // TP/RP groups
    0x08,0x0B,0x02,0x03,0x00,  // G0: 24 frames
    0x0C,0x02,0x07,0x02,0x00,  // G1: 23 frames
    0x01,0x00,0x02,0x00,0x00,  // G2:  3 frames
    0x00,0x00,0x00,0x00,0x00,  // G3
    0x00,0x00,0x00,0x00,0x00,  // G4
    0x00,0x00,0x00,0x00,0x00,  // G5
    0x00,0x00,0x00,0x00,0x00,  // G6
    0x00,0x00,0x00,0x00,0x00,  // G7
    0x00,0x00,0x00,0x00,0x00,  // G8
    0x00,0x00,0x00,0x00,0x01,  // G9

    // Frame rate
    0x22,0x22,0x22,0x22,0x22,

    // Voltages (VGH, VSH1, VSH2, VSL, VCOM)
    0x17,0x41,0xA8,0x32,0x30,
    // Reserved
    0x00,0x00,
};

// clang-format on

// ---- Refresh modes (internal) ----
enum EpdRefreshMode { EPD_FULL_REFRESH, EPD_HALF_REFRESH, EPD_FAST_REFRESH };

class EInkDisplay : public microreader::IDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = microreader::DisplayFrame::kPanelWidth;  // 800 (full panel)
  static constexpr uint16_t DISPLAY_HEIGHT = microreader::DisplayFrame::kPhysicalHeight;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = microreader::DisplayFrame::kStride;
  static constexpr uint32_t BUFFER_SIZE = microreader::DisplayFrame::kPixelBytes;

  bool isScreenOn = false;
  bool inDeepSleep_ = false;
  bool in_grayscale_mode_ = false;
  bool custom_lut_active_ = false;
  bool in_grayscale_mode() const override {
    return in_grayscale_mode_;
  }

  bool is_busy() const override {
    return gpio_get_level(EPD_BUSY) == 1;
  }

  // Custom grayscale LUT buffer (112 bytes, matches kLutSize in serial_communication.h)
  uint8_t custom_grayscale_lut_[112] = {};
  bool has_custom_grayscale_lut_ = false;

  // Custom grayscale revert LUT buffer (112 bytes)
  uint8_t custom_grayscale_revert_lut_[112] = {};
  bool has_custom_grayscale_revert_lut_ = false;
  // Set a custom grayscale revert LUT (112 bytes)
  void set_grayscale_revert_lut(const uint8_t* lut) {
    memcpy(custom_grayscale_revert_lut_, lut, 112);
    has_custom_grayscale_revert_lut_ = true;
  }

  // Clear the custom grayscale revert LUT (revert to default)
  void clear_grayscale_revert_lut() {
    has_custom_grayscale_revert_lut_ = false;
  }

  spi_device_handle_t spi_;

  void begin() {
    // GPIO outputs: CS, DC, RST
    gpio_config_t out_cfg{};
    out_cfg.pin_bit_mask = (1ULL << EPD_CS) | (1ULL << EPD_DC) | (1ULL << EPD_RST);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&out_cfg);

    // GPIO input: BUSY
    gpio_config_t in_cfg{};
    in_cfg.pin_bit_mask = (1ULL << EPD_BUSY);
    in_cfg.mode = GPIO_MODE_INPUT;
    in_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    in_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    in_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&in_cfg);

    gpio_set_level(EPD_CS, 1);
    gpio_set_level(EPD_DC, 1);

    spi_bus_config_t bus{};
    bus.mosi_io_num = EPD_MOSI;
    bus.miso_io_num = GPIO_NUM_7;  // shared with SD card
    bus.sclk_io_num = EPD_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t dev{};
    dev.clock_speed_hz = 40 * 1000 * 1000;
    dev.mode = 0;
    dev.spics_io_num = -1;
    dev.queue_size = 1;
    spi_bus_add_device(SPI2_HOST, &dev, &spi_);

    resetDisplay();
    initDisplayController(true);
  }

  // ---- microreader::IDisplay ----

  void full_refresh(const uint8_t* pixels, microreader::RefreshMode mode, bool turnOffScreen) override {
    in_grayscale_mode_ = false;  // full refresh resets display state
    wakeIfNeeded();
    waitWhileBusy();
    // Buffer covers all 800 columns; panel offset is baked into draw-function coordinates.
    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    writeRamBuffer(CMD_WRITE_RAM_BW, pixels, BUFFER_SIZE);
    writeRamBuffer(CMD_WRITE_RAM_RED, pixels, BUFFER_SIZE);
    refreshDisplay(mode == microreader::RefreshMode::Half ? EPD_HALF_REFRESH : EPD_FULL_REFRESH, turnOffScreen);
  }

  void partial_refresh(const uint8_t* new_pixels, const uint8_t* prev_pixels) override {
    if (in_grayscale_mode_) {
      grayscale_revert_();
      wakeIfNeeded();
      waitWhileBusy();
      setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
      writeRamBuffer(CMD_WRITE_RAM_RED, prev_pixels, BUFFER_SIZE);
    }

    wakeIfNeeded();
    waitWhileBusy();
    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    writeRamBuffer(CMD_WRITE_RAM_BW, new_pixels, BUFFER_SIZE);
    refreshDisplay(EPD_FAST_REFRESH);
  }

  void write_ram_bw(const uint8_t* data) override {
    wakeIfNeeded();
    waitWhileBusy();
    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    writeRamBuffer(CMD_WRITE_RAM_BW, data, BUFFER_SIZE);
  }

  void write_ram_red(const uint8_t* data) override {
    wakeIfNeeded();
    waitWhileBusy();
    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    writeRamBuffer(CMD_WRITE_RAM_RED, data, BUFFER_SIZE);
  }

  // Set a custom grayscale LUT (112 bytes)
  void set_grayscale_lut(const uint8_t* lut) {
    memcpy(custom_grayscale_lut_, lut, 112);
    has_custom_grayscale_lut_ = true;
  }

  // Clear the custom grayscale LUT (revert to default)
  void clear_grayscale_lut() {
    has_custom_grayscale_lut_ = false;
  }

  void grayscale_refresh(bool turnOffScreen = false) override {
    in_grayscale_mode_ = true;
    if (has_custom_grayscale_lut_) {
      setCustomLUT_(custom_grayscale_lut_);
    } else {
      setCustomLUT_(kLutGrayscale);
    }
    refreshDisplay(EPD_FAST_REFRESH, turnOffScreen);
    setCustomLUT_(nullptr);  // clear flag; OTP LUT restored on next normal refresh
  }

  void grayscale_refresh_1pass(bool turnOffScreen = false) override {
    setCustomLUT_(kLutFactoryQuality);
    refreshDisplay(EPD_FAST_REFRESH, turnOffScreen);
    setCustomLUT_(nullptr);
  }

  void revert_grayscale(const uint8_t* prev_pixels) override {
    if (!in_grayscale_mode_)
      return;
    grayscale_revert_();
    // Re-upload the previous BW frame so the controller's RED RAM matches
    // what was on screen before grayscale, enabling correct partial update.
    wakeIfNeeded();
    waitWhileBusy();
    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    writeRamBuffer(CMD_WRITE_RAM_RED, prev_pixels, BUFFER_SIZE);
  }

  void deep_sleep() override {
    sendCommand(CMD_DEEP_SLEEP);
    sendData(0x03);
    isScreenOn = false;
    inDeepSleep_ = true;
  }

  // Partially refresh a physical sub-rectangle.
  // phys_x is a raw hardware column (0 = first panel column); caller is responsible for
  // adding any panel offset before calling. phys_x must be byte-aligned (multiple of 8).
  void partial_refresh_region(int phys_x, int phys_y, int phys_w, int phys_h, const uint8_t* new_buf,
                              int stride_bytes) override {
    const uint16_t x_start = static_cast<uint16_t>(phys_x);
    const uint32_t total_bytes = static_cast<uint32_t>(stride_bytes * phys_h);
    wakeIfNeeded();
    waitWhileBusy();  // wait for any previous refresh to finish before writing RAM
    setRamArea(x_start, static_cast<uint16_t>(phys_y), static_cast<uint16_t>(phys_w), static_cast<uint16_t>(phys_h));
    writeRamBuffer(CMD_WRITE_RAM_BW, new_buf, total_bytes);
    // Fire the refresh and return immediately — don't block waiting for the panel.
    // The next waitWhileBusy() call (at the start of this function) will catch up.
    sendCommand(CMD_DISPLAY_UPDATE_CTRL1);
    sendData(CTRL1_NORMAL);
    sendCommand(CMD_DISPLAY_UPDATE_CTRL2);
    sendData(custom_lut_active_ ? 0x0C : 0x1C);  // EPD_FAST_REFRESH bits, no screen power change
    sendCommand(CMD_MASTER_ACTIVATION);
    // No waitWhileBusy() here — conversion continues while panel refreshes.
  }

 private:
  // Exit deep sleep via hardware reset + controller re-init.
  void wakeIfNeeded() {
    if (!inDeepSleep_)
      return;
    ESP_LOGI("epd", "waking from deep sleep (HWRESET)");
    gpio_set_level(EPD_RST, 1);
    delay(10);
    gpio_set_level(EPD_RST, 0);
    delay(20);
    gpio_set_level(EPD_RST, 1);
    delay(200);
    waitWhileBusy("post-HWRESET");
    initDisplayController(false);
    inDeepSleep_ = false;
  }

  static uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000);
  }
  static void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
  }

  void sendCommand(uint8_t command) {
    gpio_set_level(EPD_DC, 0);
    gpio_set_level(EPD_CS, 0);
    spi_transaction_t t{};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = command;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(EPD_CS, 1);
  }

  void sendData(uint8_t data) {
    gpio_set_level(EPD_DC, 1);
    gpio_set_level(EPD_CS, 0);
    spi_transaction_t t{};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = data;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(EPD_CS, 1);
  }

  void sendData(const uint8_t* data, uint32_t length) {
    static constexpr size_t kChunk = 4092;
    gpio_set_level(EPD_DC, 1);
    size_t offset = 0;
    while (offset < length) {
      const size_t chunk = (length - offset < kChunk) ? (length - offset) : kChunk;
      gpio_set_level(EPD_CS, 0);
      spi_transaction_t t{};
      t.length = chunk * 8;
      t.tx_buffer = data + offset;
      spi_device_polling_transmit(spi_, &t);
      gpio_set_level(EPD_CS, 1);
      offset += chunk;
    }
  }

  void waitWhileBusy(const char* comment = nullptr) {
    uint32_t start = millis();
    while (gpio_get_level(EPD_BUSY) == 1) {
      vTaskDelay(pdMS_TO_TICKS(1));
      if (millis() - start > 10000) {
        ESP_LOGW("epd", "waitWhileBusy timeout%s", comment ? comment : "");
        break;
      }
    }
    if (comment) {
      ESP_LOGI("epd", "waitWhileBusy done: %s (%lu ms)", comment, millis() - start);
    }
  }

  void resetDisplay() {
    gpio_set_level(EPD_RST, 1);
    delay(20);
    gpio_set_level(EPD_RST, 0);
    delay(2);
    gpio_set_level(EPD_RST, 1);
    delay(20);
  }

  void refreshDisplay(EpdRefreshMode mode, bool turnOffScreen = false) {
    sendCommand(CMD_DISPLAY_UPDATE_CTRL1);
    sendData(mode == EPD_FAST_REFRESH ? CTRL1_NORMAL : CTRL1_BYPASS_RED);

    // best guess at display mode bits:
    // bit | hex | name                    | effect
    // ----+-----+--------------------------+-------------------------------------------
    // 7   | 80  | CLOCK_ON                | Start internal oscillator
    // 6   | 40  | ANALOG_ON               | Enable analog power rails (VGH/VGL drivers)
    // 5   | 20  | TEMP_LOAD               | Load temperature (internal or I2C)
    // 4   | 10  | LUT_LOAD                | Load waveform LUT
    // 3   | 08  | MODE_SELECT             | Mode 1/2
    // 2   | 04  | DISPLAY_START           | Run display
    // 1   | 02  | ANALOG_OFF_PHASE        | Shutdown step 1 (undocumented)
    // 0   | 01  | CLOCK_OFF               | Disable internal oscillator

    uint8_t displayMode = 0x00;
    if (!isScreenOn) {
      isScreenOn = true;
      displayMode |= 0xC0;  // CLOCK_ON + ANALOG_ON (power up for first refresh)
    }

    if (turnOffScreen) {
      isScreenOn = false;
      displayMode |= 0x03;  // Set ANALOG_OFF_PHASE and CLOCK_OFF bits
    }

    switch (mode) {
      case EPD_FULL_REFRESH:
        displayMode |= 0x34;
        break;
      case EPD_HALF_REFRESH:
        sendCommand(CMD_WRITE_TEMP);
        sendData(0x5A);
        displayMode |= 0xD4;
        break;
      case EPD_FAST_REFRESH:
        // Skip LUT_LOAD (bit 4) when custom LUT is active so it isn't overwritten by OTP.
        displayMode |= custom_lut_active_ ? 0x0C : 0x1C;
        break;
    }

    sendCommand(CMD_DISPLAY_UPDATE_CTRL2);
    sendData(displayMode);

    sendCommand(CMD_MASTER_ACTIVATION);

    const uint32_t t0 = millis();
    waitWhileBusy();
    ESP_LOGI("epd", "refreshDisplay: mode=%s displayMode=0x%02X duration=%lums",
             mode == EPD_FULL_REFRESH   ? "FULL"
             : mode == EPD_HALF_REFRESH ? "HALF"
                                        : "FAST",
             displayMode, millis() - t0);
  }

  void initDisplayController(bool clearBuffer) {
    sendCommand(CMD_SOFT_RESET);
    waitWhileBusy("CMD_SOFT_RESET");

    sendCommand(CMD_TEMP_SENSOR_CONTROL);
    sendData(0x80);

    sendCommand(CMD_BOOSTER_SOFT_START);
    sendData(0xAE);
    sendData(0xC7);
    sendData(0xC3);
    sendData(0xC0);
    sendData(0x40);

    sendCommand(CMD_DRIVER_OUTPUT_CONTROL);
    sendData((DISPLAY_HEIGHT - 1) % 256);
    sendData((DISPLAY_HEIGHT - 1) / 256);
    sendData(0x02);

    sendCommand(CMD_BORDER_WAVEFORM);
    sendData(0x01);

    if (clearBuffer) {
      clearDisplay();
    }

    ESP_LOGI("epd", "finish initDisplayController: clearBuffer=%d", clearBuffer);
  }

  void clearDisplay() {
    sendCommand(CMD_AUTO_WRITE_BW_RAM);
    sendData(0x77);
    waitWhileBusy("CMD_AUTO_WRITE_BW_RAM");

    sendCommand(CMD_AUTO_WRITE_RED_RAM);
    sendData(0x77);
    waitWhileBusy("CMD_AUTO_WRITE_RED_RAM");
  }

  void setRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    y = DISPLAY_HEIGHT - y - h;

    sendCommand(CMD_DATA_ENTRY_MODE);
    sendData(0x01);  // X inc, Y dec

    sendCommand(CMD_SET_RAM_X_RANGE);
    sendData(x % 256);
    sendData(x / 256);
    sendData((x + w - 1) % 256);
    sendData((x + w - 1) / 256);

    sendCommand(CMD_SET_RAM_Y_RANGE);
    sendData((y + h - 1) % 256);
    sendData((y + h - 1) / 256);
    sendData(y % 256);
    sendData(y / 256);

    sendCommand(CMD_SET_RAM_X_COUNTER);
    sendData(x % 256);
    sendData(x / 256);

    sendCommand(CMD_SET_RAM_Y_COUNTER);
    sendData((y + h - 1) % 256);
    sendData((y + h - 1) / 256);
  }

  void writeRamBuffer(uint8_t ramBuffer, const uint8_t* data, uint32_t size) {
    sendCommand(ramBuffer);
    sendData(data, size);
  }

  // Load a custom LUT into the SSD1677's waveform register, or clear the flag.
  void setCustomLUT_(const uint8_t* lut_data) {
    if (lut_data) {
      // Bytes 0..104: waveform + timing + frame rate → CMD_WRITE_LUT (0x32)
      sendCommand(CMD_WRITE_LUT);
      sendData(lut_data, 105);
      // Byte 105: VGH
      sendCommand(CMD_GATE_VOLTAGE);
      sendData(lut_data[105]);
      // Bytes 106..108: VSH1, VSH2, VSL
      sendCommand(CMD_SOURCE_VOLTAGE);
      sendData(lut_data[106]);
      sendData(lut_data[107]);
      sendData(lut_data[108]);
      // Byte 109: VCOM
      sendCommand(CMD_WRITE_VCOM);
      sendData(lut_data[109]);
      custom_lut_active_ = true;
    } else {
      custom_lut_active_ = false;
    }
  }

  // Revert from grayscale mode: the revert LUT reads the {BW,RED} RAM
  // (still containing LSB/MSB plane data) and drives each pixel back to BW.
  void grayscale_revert_() {
    in_grayscale_mode_ = false;
    if (has_custom_grayscale_revert_lut_) {
      setCustomLUT_(custom_grayscale_revert_lut_);
    } else {
      setCustomLUT_(kLutGrayscaleRevert);
    }
    refreshDisplay(EPD_FAST_REFRESH);
    setCustomLUT_(nullptr);
  }
};