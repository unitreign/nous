#pragma once

// EInkDisplay driver for ESP-IDF.
//
// The X4 ships with an SSD1677 panel controller; newer production batches use an
// UltraChip UC8179 or UC8279 on the same pins and glass. The controller is
// identified at boot (see epd_probe below) and the matching command set is used.
// UC8179/UC8279 support is ported from the FreeInk SDK (MIT, github.com/Free-Ink/
// freeink-sdk: Uc8179Driver, Uc8279X4Driver, XteinkDetect) — black-and-white
// refreshes only; the reader's anti-aliasing grayscale stays SSD1677-only.

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
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

// ---- Panel controller detection ----
enum class EpdController : uint8_t { Unknown = 0, Ssd1677 = 1, Uc8179 = 2, Uc8279 = 3 };

// RTC memory survives deep sleep, so the probe (~70 ms of reset pulses) runs
// once per power-on rather than on every wake.
static RTC_DATA_ATTR uint8_t s_epd_controller = 0;

namespace epd_probe {
// Bit-banged half-duplex reads of the UltraChip VER (0x70) / FLG (0x71)
// registers on the display pins, before the SPI driver claims them. An SSD1677
// doesn't answer these, so its bus floats to a uniform level and is never
// mistaken for a UC part; anything inconclusive keeps the SSD1677 driver.
inline void clk_delay() { esp_rom_delay_us(1); }

inline void write_byte(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    gpio_set_level(EPD_MOSI, (b & 0x80) ? 1 : 0);
    clk_delay();
    gpio_set_level(EPD_SCLK, 1);
    clk_delay();
    gpio_set_level(EPD_SCLK, 0);
    b = static_cast<uint8_t>(b << 1);
  }
}

// The controller shifts the next bit out on the falling edge; sample while low.
inline uint8_t read_byte() {
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) {
    clk_delay();
    b = static_cast<uint8_t>((b << 1) | (gpio_get_level(EPD_MOSI) ? 1 : 0));
    gpio_set_level(EPD_SCLK, 1);
    clk_delay();
    gpio_set_level(EPD_SCLK, 0);
  }
  return b;
}

inline void cmd_read(uint8_t cmd, uint8_t* out, int len) {
  gpio_set_direction(EPD_MOSI, GPIO_MODE_OUTPUT);
  gpio_set_level(EPD_DC, 0);
  gpio_set_level(EPD_CS, 0);
  clk_delay();
  write_byte(cmd);
  gpio_set_level(EPD_DC, 1);
  gpio_set_direction(EPD_MOSI, GPIO_MODE_INPUT);
  gpio_set_pull_mode(EPD_MOSI, GPIO_PULLUP_ONLY);
  clk_delay();
  for (int i = 0; i < len; i++)
    out[i] = read_byte();
  gpio_set_level(EPD_CS, 1);
  gpio_set_pull_mode(EPD_MOSI, GPIO_FLOATING);
  gpio_set_direction(EPD_MOSI, GPIO_MODE_OUTPUT);
}

inline bool ver_floating(const uint8_t v[5]) {
  for (int i = 1; i < 5; i++)
    if (v[i] != v[0])
      return false;
  return true;
}

// One reset + FLG/VER read. A UC81xx answers FLG with a real idle status
// (BUSY_N bit set) and VER with a driven, non-uniform pattern.
inline bool pass(uint8_t ver[5], uint8_t* flg, uint32_t rst_low_us) {
  gpio_set_direction(EPD_CS, GPIO_MODE_OUTPUT);
  gpio_set_level(EPD_CS, 1);
  gpio_set_direction(EPD_SCLK, GPIO_MODE_OUTPUT);
  gpio_set_level(EPD_SCLK, 0);
  gpio_set_direction(EPD_DC, GPIO_MODE_OUTPUT);
  gpio_set_level(EPD_DC, 0);
  gpio_set_direction(EPD_MOSI, GPIO_MODE_OUTPUT);
  gpio_set_direction(EPD_BUSY, GPIO_MODE_INPUT);
  gpio_hold_dis(EPD_RST);
  gpio_set_direction(EPD_RST, GPIO_MODE_OUTPUT);
  gpio_set_level(EPD_RST, 1);
  esp_rom_delay_us(2000);
  gpio_set_level(EPD_RST, 0);
  esp_rom_delay_us(rst_low_us);
  gpio_set_level(EPD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(30));

  uint8_t f = 0;
  cmd_read(0x71, &f, 1);
  cmd_read(0x70, ver, 5);
  if (flg)
    *flg = f;
  if (f == 0x00 || f == 0xFF || (f & 0x01) == 0)
    return false;
  return !ver_floating(ver);
}

// LUT_VER (VER byte 2) values the stock firmware maps to a UC8279; any other
// confirmed UltraChip part takes the UC8179 driver (FreeInk's default).
inline bool is_uc8279_id(uint8_t id) {
  return id == 0x02 || id == 0x03 || id == 0x41 || id == 0x42 || id == 0x67 || id == 0x68 || id == 0x69;
}

// Two passes that must agree: a floating bus can't repeat a structured VER.
// The first pass uses a short reset; only a UC-looking answer pays for the
// vendor 50 ms reset on the confirming pass.
inline EpdController detect() {
  // Route the pins to plain GPIO first: GPIO21 (CS) boots as UART0 TX, and
  // gpio_set_direction() alone doesn't change a pin's IOMUX function.
  gpio_config_t io{};
  io.pin_bit_mask = (1ULL << EPD_SCLK) | (1ULL << EPD_MOSI) | (1ULL << EPD_CS) | (1ULL << EPD_DC) | (1ULL << EPD_RST);
  io.mode = GPIO_MODE_INPUT_OUTPUT;
  gpio_config(&io);
  gpio_config_t busy{};
  busy.pin_bit_mask = 1ULL << EPD_BUSY;
  busy.mode = GPIO_MODE_INPUT;
  gpio_config(&busy);

  uint8_t v1[5] = {}, v2[5] = {}, f1 = 0;
  const bool p1 = pass(v1, &f1, 1000);
  esp_rom_delay_us(2000);
  const bool p2 = pass(v2, nullptr, p1 ? 50000 : 1000);

  gpio_set_direction(EPD_SCLK, GPIO_MODE_INPUT);
  gpio_set_direction(EPD_MOSI, GPIO_MODE_INPUT);
  gpio_set_direction(EPD_DC, GPIO_MODE_INPUT);
  gpio_set_direction(EPD_RST, GPIO_MODE_INPUT);  // RST_N has an internal pull-up
  gpio_set_level(EPD_CS, 1);

  const bool confirmed = p1 && p2 && std::memcmp(v1, v2, 5) == 0;
  ESP_LOGI("epd", "controller probe: FLG=%02X VER=%02X %02X %02X %02X %02X -> %s", f1, v1[0], v1[1], v1[2], v1[3],
           v1[4], confirmed ? (is_uc8279_id(v2[2]) ? "UC8279" : "UC8179") : "SSD1677");
  if (!confirmed)
    return EpdController::Ssd1677;
  return is_uc8279_id(v2[2]) ? EpdController::Uc8279 : EpdController::Uc8179;
}
}  // namespace epd_probe

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
    // SSD1677 BUSY is active-high; the UltraChip BUSY_N is low while busy.
    return gpio_get_level(EPD_BUSY) == (is_uc_() ? 0 : 1);
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
    if (s_epd_controller == 0)
      s_epd_controller = static_cast<uint8_t>(epd_probe::detect());
    ctrl_ = static_cast<EpdController>(s_epd_controller);

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
    // SSD1677: 20 MHz, half the old 40 MHz overclock (the FreeInk SDK uses
    // 10 MHz for every Xteink panel); a 48 KB plane takes ~19 ms instead of
    // ~10 ms, small next to the 300+ ms waveform. UC8179/UC8279: 10 MHz, the
    // value they were brought up at.
    dev.clock_speed_hz = (ctrl_ == EpdController::Ssd1677 ? 20 : 10) * 1000 * 1000;
    dev.mode = 0;
    dev.spics_io_num = -1;
    dev.queue_size = 1;
    spi_bus_add_device(SPI2_HOST, &dev, &spi_);

    if (is_uc_()) {
      uc_init_();
      return;
    }
    resetDisplay();
    initDisplayController(true);
  }

  EpdController controller() const {
    return ctrl_;
  }

  bool supports_grayscale() const override {
    return !is_uc_();
  }

  // ---- microreader::IDisplay ----

  void full_refresh(const uint8_t* pixels, microreader::RefreshMode mode, bool turnOffScreen) override {
    if (is_uc_()) {
      uc_show_(uc_row_fb_, pixels, mode == microreader::RefreshMode::Half ? EPD_HALF_REFRESH : EPD_FULL_REFRESH,
               turnOffScreen);
      return;
    }
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
    if (is_uc_()) {
      // The controller keeps the displayed frame in its OLD plane itself.
      uc_show_(uc_row_fb_, new_pixels, EPD_FAST_REFRESH, false);
      return;
    }
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
    if (is_uc_())
      return;  // UC panels show only the second plane (see write_ram_red)
    wakeIfNeeded();
    waitWhileBusy();
    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    writeRamBuffer(CMD_WRITE_RAM_BW, data, BUFFER_SIZE);
  }

  void write_ram_red(const uint8_t* data) override {
    if (is_uc_()) {
      // No grayscale waveform on UC panels: the second plane (white for the
      // light and white levels of a 4-level image) becomes a 2-tone frame,
      // shown by the next grayscale_refresh*().
      uc_prepare_();
      uc_stream_(UC_DTM2, uc_row_fb_, data, false);
      uc_gray_pending_ = true;
      return;
    }
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
    if (is_uc_()) {
      uc_show_gray_fallback_(turnOffScreen);
      return;
    }
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
    if (is_uc_()) {
      uc_show_gray_fallback_(turnOffScreen);
      return;
    }
    setCustomLUT_(kLutFactoryQuality);
    refreshDisplay(EPD_FAST_REFRESH, turnOffScreen);
    setCustomLUT_(nullptr);
  }

  void revert_grayscale(const uint8_t* prev_pixels) override {
    if (is_uc_() || !in_grayscale_mode_)
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
    if (is_uc_()) {
      uc_settle_();
      uc_power_off_();
      sendCommand(UC_DSLP);
      sendData(0xA5);
      uc_sleeping_ = true;
      return;
    }
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
    if (is_uc_()) {
      uc_show_region_(phys_x, phys_y, phys_w, phys_h, new_buf, stride_bytes);
      return;
    }
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

  // The SD card shares SPI2_HOST and is driven from other tasks (serial
  // uploads). Display CS is a plain GPIO the SPI driver doesn't know about, so
  // the bus is held for the whole time CS is low; otherwise an SD transaction
  // could run while the panel is selected and the panel would latch SD bytes.
  struct BusLock {
    spi_device_handle_t dev;
    explicit BusLock(spi_device_handle_t d) : dev(d) { spi_device_acquire_bus(dev, portMAX_DELAY); }
    ~BusLock() { spi_device_release_bus(dev); }
  };

  void sendCommand(uint8_t command) {
    BusLock lock(spi_);
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
    BusLock lock(spi_);
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
    BusLock lock(spi_);
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
    // Short busy periods (RAM/LUT commands) end within a few ms, so poll those
    // tightly. Refreshes take hundreds of ms: after the first 2 ms block a full
    // tick per poll (pdMS_TO_TICKS(1) is 0 at 100 Hz, which only spun the CPU
    // and starved the idle task for the whole refresh).
    while (gpio_get_level(EPD_BUSY) == 1) {
      if (millis() - start < 2)
        esp_rom_delay_us(50);
      else
        vTaskDelay(1);
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

  // ======================================================================
  // UltraChip UC8179 / UC8279 (newer X4 batches)
  // ======================================================================
  // KW mode, 1 bpp: DTM1 = OLD plane, DTM2 = NEW plane, and the (old, new) pair
  // picks each pixel's waveform. The OLD plane is kept equal to the displayed
  // frame after every refresh, so a fast refresh drives only changed pixels.
  // The panel is addressed as 800x600 gates with 480 visible. Register values
  // and ordering follow FreeInk's Uc8179Driver / Uc8279X4Driver (stock firmware).

  static constexpr uint8_t UC_PSR = 0x00;
  static constexpr uint8_t UC_POF = 0x02;
  static constexpr uint8_t UC_PFS = 0x03;
  static constexpr uint8_t UC_PON = 0x04;
  static constexpr uint8_t UC_BTST = 0x06;
  static constexpr uint8_t UC_DSLP = 0x07;
  static constexpr uint8_t UC_DTM1 = 0x10;
  static constexpr uint8_t UC_DRF = 0x12;
  static constexpr uint8_t UC_DTM2 = 0x13;
  static constexpr uint8_t UC_PLL = 0x30;
  static constexpr uint8_t UC_CDI = 0x50;
  static constexpr uint8_t UC_TRES = 0x61;
  static constexpr uint8_t UC_GSST = 0x65;
  static constexpr uint8_t UC_PTL = 0x90;
  static constexpr uint8_t UC_PTIN = 0x91;
  static constexpr uint8_t UC_PTOUT = 0x92;
  static constexpr uint8_t UC_CCSET = 0xE0;
  static constexpr uint8_t UC_GATE_SCAN = 0xE1;
  static constexpr uint8_t UC_PWS = 0xE3;
  static constexpr uint8_t UC_TSSET = 0xE5;

  static constexpr uint16_t UC_TRES_H = 600;      // addressed gate count
  static constexpr uint16_t UC8279_GATE_OFS = 120;  // UC8279: first visible gate
  static constexpr int UC_WB = DISPLAY_WIDTH_BYTES;

  // A row source: fill `out` (UC_WB bytes) with framebuffer row y.
  using UcRowFn = void (*)(const void* ctx, int y, uint8_t* out);

  struct UcRegion {
    int x_byte, y, w_bytes, h, stride;
    const uint8_t* buf;
  };

  EpdController ctrl_ = EpdController::Ssd1677;
  bool uc_on_ = false;           // analog power on (PON sent)
  bool uc_sleeping_ = false;     // DSLP sent; needs reset + init
  bool uc_need_full_ = true;     // next refresh must be a full (clearing) one
  bool uc_old_valid_ = false;    // OLD plane holds the displayed frame
  bool uc_gray_pending_ = false; // write_ram_red() loaded a frame awaiting refresh
  bool uc_pending_ = false;      // a region refresh is still running
  bool uc_pending_fast_ = false;
  bool uc_region_valid_ = false; // uc_region_prev_ holds the last loading box
  uint8_t uc_region_prev_[1024] = {};

  bool is_uc_() const {
    return ctrl_ == EpdController::Uc8179 || ctrl_ == EpdController::Uc8279;
  }

  static void uc_row_fb_(const void* ctx, int y, uint8_t* out) {
    std::memcpy(out, static_cast<const uint8_t*>(ctx) + static_cast<size_t>(y) * UC_WB, UC_WB);
  }
  static void uc_row_white_(const void*, int, uint8_t* out) {
    std::memset(out, 0xFF, UC_WB);
  }
  // A white frame with a small region (the loading box) copied in.
  static void uc_row_region_(const void* ctx, int y, uint8_t* out) {
    const auto* r = static_cast<const UcRegion*>(ctx);
    std::memset(out, 0xFF, UC_WB);
    if (y >= r->y && y < r->y + r->h)
      std::memcpy(out + r->x_byte, r->buf + static_cast<size_t>(y - r->y) * r->stride, r->w_bytes);
  }

  // BUSY_N is low while busy. Like the stock firmware, allow one tick for the
  // command to take effect, then wait for the idle level; the controller drops
  // RAM/LUT writes issued while busy.
  void uc_wait_busy_(const char* tag) {
    vTaskDelay(1);
    const uint32_t start = millis();
    while (gpio_get_level(EPD_BUSY) == 0) {
      vTaskDelay(1);
      if (millis() - start > 30000) {
        ESP_LOGW("epd", "UC busy timeout: %s", tag);
        break;
      }
    }
  }

  void uc_init_() {
    gpio_set_level(EPD_RST, 1);
    delay(10);
    gpio_set_level(EPD_RST, 0);
    delay(10);
    gpio_set_level(EPD_RST, 1);
    delay(60);

    const bool c8179 = ctrl_ == EpdController::Uc8179;
    sendCommand(UC_PSR);
    sendData(c8179 ? 0x3F : 0x37);
    sendData(c8179 ? 0x0A : 0x4D);
    sendCommand(UC_TRES);  // 800 x 600 (480 gates visible)
    sendData(DISPLAY_WIDTH >> 8);
    sendData(DISPLAY_WIDTH & 0xFF);
    sendData(UC_TRES_H >> 8);
    sendData(UC_TRES_H & 0xFF);
    sendCommand(UC_GSST);
    for (int i = 0; i < 4; i++)
      sendData(0x00);
    sendCommand(UC_PFS);
    sendData(0x20);
    if (c8179) {
      sendCommand(UC_BTST);
      sendData(0x25);
      sendData(0x25);
      sendData(0x3C);
      sendData(0x25);
    } else {
      sendCommand(UC_PLL);
      sendData(0x0E);
    }
    sendCommand(UC_GATE_SCAN);
    sendData(0x02);
    if (c8179) {
      sendCommand(UC_PWS);  // GxEPD2's UC8179 value against dithered-image artifacts
      sendData(0x22);
    }
    uc_on_ = false;
    uc_sleeping_ = false;
    uc_need_full_ = true;
    uc_old_valid_ = false;
    uc_region_valid_ = false;
    ESP_LOGI("epd", "UC%s initialised", c8179 ? "8179" : "8279");
  }

  // Stream one RAM plane: visible rows in the controller's order plus white
  // padding up to the 600 addressed gates. UC8179 takes rows bottom-up; UC8279
  // takes them top-down after a 120-gate offset. Rows are batched into one
  // CS-low burst so the plane isn't split into hundreds of transactions.
  void uc_stream_(uint8_t ram_cmd, UcRowFn fn, const void* ctx, bool invert) {
    static WORD_ALIGNED_ATTR uint8_t stage[UC_WB * 20];
    sendCommand(ram_cmd);
    BusLock lock(spi_);
    gpio_set_level(EPD_DC, 1);
    gpio_set_level(EPD_CS, 0);
    size_t fill = 0;
    auto flush = [&]() {
      if (fill == 0)
        return;
      spi_transaction_t t{};
      t.length = fill * 8;
      t.tx_buffer = stage;
      spi_device_polling_transmit(spi_, &t);
      fill = 0;
    };
    auto emit = [&](int y) {  // y < 0: white padding row
      uint8_t* out = stage + fill;
      if (y < 0) {
        std::memset(out, 0xFF, UC_WB);
      } else {
        fn(ctx, y, out);
        if (invert)
          for (int i = 0; i < UC_WB; i++)
            out[i] = static_cast<uint8_t>(~out[i]);
      }
      fill += UC_WB;
      if (fill + UC_WB > sizeof(stage))
        flush();
    };
    const int h = DISPLAY_HEIGHT;
    if (ctrl_ == EpdController::Uc8179) {
      for (int y = h - 1; y >= 0; --y)
        emit(y);
      for (int g = h; g < UC_TRES_H; ++g)
        emit(-1);
    } else {
      for (int g = 0; g < UC8279_GATE_OFS; ++g)
        emit(-1);
      for (int y = 0; y < h; ++y)
        emit(y);
      for (int g = UC8279_GATE_OFS + h; g < UC_TRES_H; ++g)
        emit(-1);
    }
    flush();
    gpio_set_level(EPD_CS, 1);
  }

  // Configure and start a refresh (stock register order), returning once the
  // waveform has started. `fast` = differential partial (DU); else GC full.
  void uc_start_refresh_(bool fast) {
    const bool c8179 = ctrl_ == EpdController::Uc8179;
    sendCommand(UC_CDI);
    if (c8179) {
      sendData(0x29);
      sendData(0x07);
    } else {
      sendData(fast ? 0xD7 : 0x97);  // UC8279 CDI is a single byte
    }
    sendCommand(UC_CCSET);
    sendData(0x02);
    sendCommand(UC_TSSET);
    sendData(fast ? 0x5A : 0x1E);
    if (c8179) {
      sendCommand(UC_PSR);  // REG bit cleared -> OTP waveforms
      sendData(0x3F & 0xDF);
      sendData(0x0A);
    }
    if (fast) {
      sendCommand(UC_PFS);
      sendData(0x20);
      sendCommand(UC_GATE_SCAN);
      sendData(0x02);
    }
    if (!uc_on_) {
      sendCommand(UC_PON);
      uc_wait_busy_("PON");
      uc_on_ = true;
    }
    if (fast) {
      sendCommand(UC_PTIN);
      if (!c8179) {
        // UC8279 develops nothing on a partial without a PTL window.
        const uint16_t x_end = DISPLAY_WIDTH - 1;
        const uint16_t y0 = UC8279_GATE_OFS;
        const uint16_t y1 = UC8279_GATE_OFS + DISPLAY_HEIGHT - 1;
        sendCommand(UC_PTL);
        sendData(0x00);
        sendData(0x00);
        sendData(x_end >> 8);
        sendData((x_end & 0xFF) | 0x07);
        sendData(y0 >> 8);
        sendData(y0 & 0xFF);
        sendData(y1 >> 8);
        sendData(y1 & 0xFF);
        sendData(0x01);
      }
    }
    if (!c8179) {
      sendCommand(UC_PSR);  // UC8279 latches PSR only after PON
      sendData(0x37 & 0xDF);
      sendData(0x4D);
    }
    sendCommand(UC_DRF);
    const uint32_t t0 = millis();
    while (gpio_get_level(EPD_BUSY) == 1 && millis() - t0 < 50)
      vTaskDelay(1);
  }

  void uc_finish_refresh_(bool fast) {
    uc_wait_busy_("DRF");
    if (fast)
      sendCommand(UC_PTOUT);
    if (ctrl_ == EpdController::Uc8179) {
      sendCommand(UC_CDI);  // idle border value
      sendData(0xA9);
      sendData(0x07);
    }
  }

  void uc_power_off_() {
    if (!uc_on_)
      return;
    sendCommand(UC_POF);
    uc_wait_busy_("POF");
    uc_on_ = false;
  }

  // Finish a region refresh still in flight.
  void uc_settle_() {
    if (!uc_pending_)
      return;
    uc_finish_refresh_(uc_pending_fast_);
    uc_pending_ = false;
  }

  void uc_prepare_() {
    uc_settle_();
    if (uc_sleeping_)
      uc_init_();
  }

  // Show a full frame. Full: absolute refresh from a white OLD plane. Half:
  // OLD = ~target, so every pixel transitions (a charge scrub). Fast: diff
  // against the displayed frame in the OLD plane.
  void uc_show_(UcRowFn fn, const void* ctx, EpdRefreshMode mode, bool turn_off) {
    uc_prepare_();
    const bool fast = mode == EPD_FAST_REFRESH && !uc_need_full_ && uc_old_valid_;
    uc_stream_(UC_DTM2, fn, ctx, false);
    if (!fast) {
      if (mode == EPD_HALF_REFRESH)
        uc_stream_(UC_DTM1, fn, ctx, true);
      else
        uc_stream_(UC_DTM1, uc_row_white_, nullptr, false);
    }
    uc_start_refresh_(fast);
    uc_finish_refresh_(fast);
    uc_stream_(UC_DTM1, fn, ctx, false);
    uc_old_valid_ = true;
    uc_need_full_ = false;
    uc_region_valid_ = false;
    uc_gray_pending_ = false;
    if (turn_off)
      uc_power_off_();
  }

  void uc_show_gray_fallback_(bool turn_off) {
    if (!uc_gray_pending_)
      return;
    uc_settle_();
    uc_stream_(UC_DTM1, uc_row_white_, nullptr, false);
    uc_start_refresh_(false);
    uc_finish_refresh_(false);
    uc_gray_pending_ = false;
    uc_old_valid_ = false;  // the frame came from a temporary plane
    uc_need_full_ = true;
    if (turn_off)
      uc_power_off_();
  }

  // Loading box while both frame buffers are in use as conversion scratch:
  // there is no full frame to diff against, so the panel shows white plus the
  // box. The first call is a full refresh; later ones diff against the
  // previous box. Returns while the refresh runs (is_busy() reports it).
  void uc_show_region_(int phys_x, int phys_y, int phys_w, int phys_h, const uint8_t* buf, int stride) {
    uc_prepare_();
    const int x_byte = phys_x / 8;
    UcRegion now{x_byte, phys_y, std::min(stride, UC_WB - x_byte), phys_h, stride, buf};
    (void)phys_w;
    const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(phys_h);
    const bool fits = bytes <= sizeof(uc_region_prev_);
    const bool fast = uc_region_valid_ && fits;
    uc_stream_(UC_DTM2, uc_row_region_, &now, false);
    if (fast) {
      UcRegion prev = now;
      prev.buf = uc_region_prev_;
      uc_stream_(UC_DTM1, uc_row_region_, &prev, false);
    } else {
      uc_stream_(UC_DTM1, uc_row_white_, nullptr, false);
    }
    if (fits)
      std::memcpy(uc_region_prev_, buf, bytes);
    uc_region_valid_ = fits;
    uc_start_refresh_(fast);
    uc_pending_ = true;
    uc_pending_fast_ = fast;
    uc_old_valid_ = false;  // the panel now shows a synthesized frame
    uc_need_full_ = true;
  }
};