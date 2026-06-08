#pragma once

// Receives frames over USB Serial/JTAG:
//   - LUT frames (magic 0xDEADBEEF) → applied to the EPD
//   - EPUB uploads (magic "EPUB")    → written to /sdcard/books/
//
// LUT frame format (little-endian):
//   [0xDE 0xAD 0xBE 0xEF]  magic (4 bytes)
//   [length]                payload length (4 bytes LE)
//   [payload]               LUT bytes (112 bytes)
//   [crc32]                 CRC-32 of payload (4 bytes LE)
//
// EPUB upload format (little-endian):
//   [0x45 0x50 0x55 0x42]  magic "EPUB" (4 bytes)
//   [name_len]             filename length (2 bytes LE)
//   [filename]             UTF-8 filename (N bytes)
//   [file_size]            payload size (4 bytes LE)
//   [payload]              file data (S bytes)
//   [crc32]                CRC-32 of payload (4 bytes LE)
//
// Device responses for EPUB upload:
//   "READY\n"  after parsing header
//   "OK\n"     on success
//   "ERR:...\n" on failure
//
// Call serial_start() once from app_main.
// Poll serial_lut_take(buf) each loop for LUT data.

#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <queue>
#include <string>

#include "microreader/content/BookIndex.h"

#ifdef QEMU_BUILD
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#else
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "font_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr const char* kLutRxTag = "lut_rx";
static constexpr const char* kUpTag = "upload";
static constexpr uint8_t kLutMagic[4] = {0xDE, 0xAD, 0xBE, 0xEF};
static constexpr uint8_t kEpubMagic[4] = {'E', 'P', 'U', 'B'};
static constexpr uint8_t kSimgMagic[4] = {'S', 'I', 'M', 'G'};
static constexpr uint8_t kSdFntMagic[4] = {'S', 'D', 'F', 'N'};
static constexpr uint8_t kCmdMagic[4] = {'C', 'M', 'N', 'D'};
static constexpr uint8_t kFontMagic[4] = {'F', 'O', 'N', 'T'};
static constexpr uint32_t kMaxPayload = 256;
static constexpr uint32_t kLutSize = 112;
static constexpr uint32_t kLutFrameSize = 113;  // 1 byte type + 112 bytes LUT

// LUT state: shared between receiver task and main loop.
static uint8_t g_lut_buf[kLutSize];
static uint8_t g_lut_type = 0;
static volatile bool g_lut_pending = false;

// Button injection: OR'd into next poll_buttons before clearing.
volatile uint8_t g_serial_buttons = 0;

// Single-slot command queue: only one path command can be pending at a time.
// The serial receiver task writes path then sets type as the commit signal.
// The main loop reads type, dispatches, then clears to None.
enum class SerialCmdType : uint8_t {
  None = 0,
  Open,
  Bench,
  ImgBench,
  ImgDecode,
  FlashBench,
  InvalidateFont,
  RenderBench
};
static char g_cmd_path[256];
static volatile SerialCmdType g_cmd_type = SerialCmdType::None;

// Set when a font has been uploaded to the partition and needs re-mmap.
static volatile bool g_font_uploaded = false;

// Call from the main loop. Returns true (and copies into `out`) when a fresh
// LUT has been received since the last call.
// Returns true and sets *type_out if a new LUT is available.
inline bool serial_lut_take(uint8_t* out, uint8_t* type_out = nullptr) {
  if (!g_lut_pending)
    return false;
  memcpy(out, g_lut_buf, kLutSize);
  if (type_out)
    *type_out = g_lut_type;
  g_lut_pending = false;
  return true;
}

// Call from the main loop. Returns the command type and sets *path_out to the
// path string. Returns None (and leaves *path_out unchanged) if nothing pending.
// Clears the pending state before returning.
inline SerialCmdType serial_cmd_take(const char** path_out) {
  SerialCmdType t = g_cmd_type;
  if (t == SerialCmdType::None)
    return SerialCmdType::None;
  if (path_out)
    *path_out = g_cmd_path;
  g_cmd_type = SerialCmdType::None;
  return t;
}

// Read exactly `n` bytes with a timeout. Returns true on success.
static bool serial_read_exact(uint8_t* buf, size_t n, uint32_t timeout_ms) {
  size_t received = 0;
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  while (received < n) {
    const TickType_t now = xTaskGetTickCount();
    if ((int32_t)(deadline - now) <= 0)
      return false;
#ifdef QEMU_BUILD
    const int r = uart_read_bytes(UART_NUM_0, buf + received, n - received, deadline - now);
#else
    const int r = usb_serial_jtag_read_bytes(buf + received, n - received, deadline - now);
#endif
    if (r > 0)
      received += r;
  }
  return true;
}

static void serial_write(const char* msg) {
#ifdef QEMU_BUILD
  uart_write_bytes(UART_NUM_0, msg, strlen(msg));
#else
  usb_serial_jtag_write_bytes((const uint8_t*)msg, strlen(msg), pdMS_TO_TICKS(1000));
#endif
}

static void serial_write_raw(const uint8_t* buf, size_t n) {
#ifdef QEMU_BUILD
  uart_write_bytes(UART_NUM_0, buf, n);
#else
  usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(1000));
#endif
}

// ---------------------------------------------------------------------------
// Handle an incoming LUT frame (after magic has been matched).
// ---------------------------------------------------------------------------
static void handle_lut_frame() {
  uint8_t len_buf[4];
  if (!serial_read_exact(len_buf, 4, 500)) {
    ESP_LOGW(kLutRxTag, "timeout reading length");
    return;
  }
  const uint32_t length =
      (uint32_t)len_buf[0] | ((uint32_t)len_buf[1] << 8) | ((uint32_t)len_buf[2] << 16) | ((uint32_t)len_buf[3] << 24);
  if (length != kLutFrameSize) {
    ESP_LOGW(kLutRxTag, "invalid LUT frame size: %lu (expected %u)", length, kLutFrameSize);
    return;
  }

  uint8_t payload[kLutFrameSize];
  if (!serial_read_exact(payload, length, 2000)) {
    ESP_LOGW(kLutRxTag, "timeout reading payload");
    return;
  }

  uint8_t crc_buf[4];
  if (!serial_read_exact(crc_buf, 4, 500)) {
    ESP_LOGW(kLutRxTag, "timeout reading crc");
    return;
  }
  const uint32_t recv_crc =
      (uint32_t)crc_buf[0] | ((uint32_t)crc_buf[1] << 8) | ((uint32_t)crc_buf[2] << 16) | ((uint32_t)crc_buf[3] << 24);
  const uint32_t calc_crc = esp_rom_crc32_le(0, payload, length);

  if (recv_crc != calc_crc) {
    ESP_LOGE(kLutRxTag, "CRC mismatch: recv=0x%08lX calc=0x%08lX", recv_crc, calc_crc);
    return;
  }

  g_lut_type = payload[0];
  memcpy(g_lut_buf, payload + 1, kLutSize);
  ESP_LOGI(kLutRxTag, "OK: received LUT type %u (%lu bytes, CRC 0x%08lX)", g_lut_type, length, calc_crc);
  g_lut_pending = true;
}

// ---------------------------------------------------------------------------
// Handle an incoming upload to a specific directory (after magic matched).
// ---------------------------------------------------------------------------
static void handle_file_upload(const char* target_dir) {
  // Read filename length (2 bytes LE).
  uint8_t hdr[2];
  if (!serial_read_exact(hdr, 2, 2000)) {
    serial_write("ERR:header\n");
    return;
  }
  uint16_t name_len = hdr[0] | (hdr[1] << 8);
  if (name_len == 0 || name_len > 200) {
    serial_write("ERR:name_len\n");
    return;
  }

  // Read filename.
  char name[204];
  if (!serial_read_exact((uint8_t*)name, name_len, 2000)) {
    serial_write("ERR:name\n");
    return;
  }
  name[name_len] = '\0';

  // Read file size (4 bytes LE).
  uint8_t sz_buf[4];
  if (!serial_read_exact(sz_buf, 4, 2000)) {
    serial_write("ERR:size\n");
    return;
  }
  uint32_t file_size = sz_buf[0] | (sz_buf[1] << 8) | (sz_buf[2] << 16) | (sz_buf[3] << 24);

  // Build path
  char path[256];
  snprintf(path, sizeof(path), "%s/%s", target_dir, name);
  mkdir(target_dir, 0775);

  FILE* f = fopen(path, "wb");
  if (!f) {
    ESP_LOGE(kUpTag, "fopen failed: %s (errno=%d: %s)", path, errno, strerror(errno));
    serial_write("ERR:fopen\n");
    return;
  }

  // Log before READY so the host's readline loop can skip it.
  ESP_LOGI(kUpTag, "receiving '%s' (%lu bytes)", name, (unsigned long)file_size);
  serial_write("READY\n");

  // Receive payload in chunks with flow control.
  // After each chunk is written to SD, send ACK (0x06) so the host
  // knows to send the next chunk.  This prevents RX buffer overflow
  // for large files.
  uint32_t crc = 0;
  uint32_t remaining = file_size;
  uint8_t chunk[2048];
  static constexpr uint8_t kAck = 0x06;
  while (remaining > 0) {
    size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    if (!serial_read_exact(chunk, want, 30000)) {
      ESP_LOGE(kUpTag, "timeout, %lu bytes remaining", (unsigned long)remaining);
      fclose(f);
      remove(path);
      serial_write("ERR:timeout\n");
      return;
    }
    fwrite(chunk, 1, want, f);
    crc = esp_rom_crc32_le(crc, chunk, want);
    remaining -= want;
    // Flow-control ACK — tell host to send next chunk.
    serial_write_raw(&kAck, 1);
  }
  fclose(f);

  // Verify CRC.
  uint8_t crc_buf[4];
  if (!serial_read_exact(crc_buf, 4, 2000)) {
    remove(path);
    serial_write("ERR:crc_missing\n");
    return;
  }
  uint32_t expected = crc_buf[0] | (crc_buf[1] << 8) | (crc_buf[2] << 16) | (crc_buf[3] << 24);
  if (crc != expected) {
    ESP_LOGE(kUpTag, "CRC mismatch: got 0x%08lx, expected 0x%08lx", (unsigned long)crc, (unsigned long)expected);
    remove(path);
    serial_write("ERR:crc\n");
    return;
  }

  ESP_LOGI(kUpTag, "saved %s (%lu bytes, CRC OK)", path, (unsigned long)file_size);
  serial_write("OK\n");
}

// ---------------------------------------------------------------------------
// Specific upload handlers
// ---------------------------------------------------------------------------
static void handle_epub_upload() {
  handle_file_upload("/sdcard/books");
}

static void handle_simg_upload() {
  handle_file_upload("/sdcard/sleep");
}

static void handle_sdfnt_upload() {
  handle_file_upload("/sdcard/fonts");
}

// ---------------------------------------------------------------------------
// Handle a FONT upload — write directly to the spiffs partition (raw flash).
// Protocol:
//   [4B] "FONT" magic (already consumed)
//   [4B] file_size (LE)
//   payload in 2KB chunks with ACK flow control (same as EPUB)
//   [4B] CRC32 (LE)
// ---------------------------------------------------------------------------
static void handle_font_upload() {
  // Read file size (4 bytes LE).
  uint8_t sz_buf[4];
  if (!serial_read_exact(sz_buf, 4, 2000)) {
    serial_write("ERR:size\n");
    return;
  }
  uint32_t file_size = sz_buf[0] | (sz_buf[1] << 8) | (sz_buf[2] << 16) | (sz_buf[3] << 24);

  const esp_partition_t* part = FontPartition::find();
  if (!part) {
    serial_write("ERR:no_partition\n");
    return;
  }
  if (kFontPartHeaderSize + file_size > part->size) {
    serial_write("ERR:too_large\n");
    return;
  }

  ESP_LOGI(kUpTag, "receiving font (%lu bytes) → spiffs partition", (unsigned long)file_size);

  // Erase needed sectors BEFORE signaling READY, so the host doesn't
  // start sending while we're busy erasing.
  size_t total = kFontPartHeaderSize + file_size;
  size_t erase_size = (total + 0xFFF) & ~0xFFF;
  if (esp_partition_erase_range(part, 0, erase_size) != ESP_OK) {
    serial_write("ERR:erase\n");
    return;
  }

  // Write the header first (magic + size).
  uint8_t header[kFontPartHeaderSize];
  memcpy(header, kFontMagic, 4);
  memcpy(header + 4, sz_buf, 4);
  if (esp_partition_write(part, 0, header, sizeof(header)) != ESP_OK) {
    serial_write("ERR:write_hdr\n");
    return;
  }

  // Now signal the host to start sending data.
  serial_write("READY\n");

  // Receive and write data in chunks.
  uint32_t crc = 0;
  uint32_t remaining = file_size;
  uint32_t flash_offset = kFontPartHeaderSize;
  uint8_t chunk[2048];
  static constexpr uint8_t kAck = 0x06;

  while (remaining > 0) {
    size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    if (!serial_read_exact(chunk, want, 30000)) {
      ESP_LOGE(kUpTag, "font upload timeout, %lu remaining", (unsigned long)remaining);
      serial_write("ERR:timeout\n");
      return;
    }
    crc = esp_rom_crc32_le(crc, chunk, want);

    if (esp_partition_write(part, flash_offset, chunk, want) != ESP_OK) {
      serial_write("ERR:write\n");
      return;
    }
    flash_offset += want;
    remaining -= want;
    serial_write_raw(&kAck, 1);
  }

  // Verify CRC.
  uint8_t crc_buf[4];
  if (!serial_read_exact(crc_buf, 4, 2000)) {
    serial_write("ERR:crc_missing\n");
    return;
  }
  uint32_t expected = crc_buf[0] | (crc_buf[1] << 8) | (crc_buf[2] << 16) | (crc_buf[3] << 24);
  if (crc != expected) {
    ESP_LOGE(kUpTag, "font CRC mismatch: got 0x%08lx expected 0x%08lx", (unsigned long)crc, (unsigned long)expected);
    serial_write("ERR:crc\n");
    return;
  }

  ESP_LOGI(kUpTag, "font saved to partition (%lu bytes, CRC OK)", (unsigned long)file_size);
  g_font_uploaded = true;
  serial_write("OK\n");
}

// ---------------------------------------------------------------------------
// Handle a serial command (after "CMND" magic has been matched).
//
// Sub-commands (1 byte after magic):
//   'B' + 1 byte button_mask  → inject button press(es)
//   'O' + 2 byte path_len LE + path  → open book by path
//   'S'                       → status query (responds with heap + screen info)
//   'L'                       → list books in /sdcard/books/
//   'F'                       → invalidate font partition (force re-provisioning on next book open)
// ---------------------------------------------------------------------------
static constexpr const char* kCmdTag = "cmd";

// Read a 2-byte LE path length followed by the path bytes into g_cmd_path.
// Sends an ERR: response and returns false on any failure.
static bool read_cmd_path(const char* log_label) {
  uint8_t len_buf[2];
  if (!serial_read_exact(len_buf, 2, 1000)) {
    serial_write("ERR:path_len\n");
    return false;
  }
  uint16_t path_len = len_buf[0] | (len_buf[1] << 8);
  if (path_len == 0 || path_len >= sizeof(g_cmd_path)) {
    serial_write("ERR:path_too_long\n");
    return false;
  }
  if (!serial_read_exact((uint8_t*)g_cmd_path, path_len, 2000)) {
    serial_write("ERR:path_read\n");
    return false;
  }
  g_cmd_path[path_len] = '\0';
  ESP_LOGI(kCmdTag, "%s: %s", log_label, g_cmd_path);
  return true;
}

static void handle_serial_cmd() {
  uint8_t sub;
  if (!serial_read_exact(&sub, 1, 1000)) {
    ESP_LOGW(kCmdTag, "timeout reading sub-command");
    return;
  }

  switch (sub) {
    case 'B': {
      uint8_t mask;
      if (!serial_read_exact(&mask, 1, 500)) {
        serial_write("ERR:btn_read\n");
        return;
      }
      g_serial_buttons |= mask;
      ESP_LOGI(kCmdTag, "button inject: 0x%02x", mask);
      serial_write("OK\n");
      break;
    }
    case 'O': {
      if (!read_cmd_path("open"))
        return;
      g_cmd_type = SerialCmdType::Open;
      serial_write("OK\n");
      break;
    }
    case 'S': {
      char buf[256];
      snprintf(buf, sizeof(buf), "STATUS:free=%lu,largest=%lu\n", (unsigned long)esp_get_free_heap_size(),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      serial_write(buf);
      break;
    }
    case 'L': {
      serial_write("BOOKS:\n");
      // Try in-memory index first (populated by main task after boot).
      // Fall back to reading the index file directly if not loaded yet
      // (serial task runs concurrently and may arrive before MainMenu::on_start).
      const auto& entries = microreader::BookIndex::instance().entries();
      if (!entries.empty()) {
        for (const auto& e : entries) {
          std::string out = "  " + e.path.to_string(microreader::BookIndex::instance().pool()) + "\n";
          serial_write(out.c_str());
        }
      } else {
        FILE* idx = fopen("/sdcard/.microreader/book_index.dat", "r");
        if (idx) {
          char line[1024];
          while (fgets(line, sizeof(line), idx)) {
            char* sep = strchr(line, '|');
            if (sep)
              *sep = '\0';
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
              line[--len] = '\0';
            if (len == 0)
              continue;
            char out[1040];
            snprintf(out, sizeof(out), "  %s\n", line);
            serial_write(out);
          }
          fclose(idx);
        }
      }
      serial_write("END\n");
      break;
    }
    case 'C': {
      // Delete all per-book subdirs in /sdcard/.microreader/cache/ and recreate it.
      const char* cache_dir = "/sdcard/.microreader/cache";
      DIR* dir = opendir(cache_dir);
      if (!dir) {
        mkdir(cache_dir, 0775);
        serial_write("CLEARED:0\n");
        break;
      }
      int count = 0;
      struct dirent* ent;
      char subdir[300];
      while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.')
          continue;
        snprintf(subdir, sizeof(subdir), "%s/%s", cache_dir, ent->d_name);
        DIR* sd = opendir(subdir);
        if (sd) {
          struct dirent* sf;
          char fpath[768];
          while ((sf = readdir(sd)) != nullptr) {
            if (sf->d_name[0] == '.')
              continue;
            snprintf(fpath, sizeof(fpath), "%s/%s", subdir, sf->d_name);
            if (remove(fpath) == 0)
              ++count;
          }
          closedir(sd);
        }
        rmdir(subdir);
      }
      closedir(dir);
      rmdir(cache_dir);
      mkdir(cache_dir, 0775);
      char buf[64];
      snprintf(buf, sizeof(buf), "CLEARED:%d\n", count);
      serial_write(buf);
      ESP_LOGI(kCmdTag, "cleared %d cache entries", count);
      break;
    }
    case 'Z': {
      const char* sleep_dir = "/sdcard/sleep";
      DIR* dir = opendir(sleep_dir);
      int count = 0;
      if (dir) {
        struct dirent* ent;
        char fpath[300];
        while ((ent = readdir(dir)) != nullptr) {
          if (ent->d_name[0] == '.')
            continue;
          snprintf(fpath, sizeof(fpath), "%s/%s", sleep_dir, ent->d_name);
          if (remove(fpath) == 0)
            ++count;
        }
        closedir(dir);
      }
      char buf[64];
      snprintf(buf, sizeof(buf), "CLEARED_SLEEP:%d\n", count);
      serial_write(buf);
      ESP_LOGI(kCmdTag, "cleared %d sleep images", count);
      break;
    }
    case 'Y': {
      const char* fonts_dir = "/sdcard/fonts";
      DIR* dir = opendir(fonts_dir);
      int count = 0;
      if (dir) {
        struct dirent* ent;
        char fpath[300];
        while ((ent = readdir(dir)) != nullptr) {
          if (ent->d_name[0] == '.')
            continue;
          snprintf(fpath, sizeof(fpath), "%s/%s", fonts_dir, ent->d_name);
          if (remove(fpath) == 0)
            ++count;
        }
        closedir(dir);
      }
      char buf[64];
      snprintf(buf, sizeof(buf), "CLEARED_SDFONTS:%d\n", count);
      serial_write(buf);
      ESP_LOGI(kCmdTag, "cleared %d SD fonts", count);
      break;
    }
    case 'R': {
      if (!read_cmd_path("rm"))
        return;
      if (remove(g_cmd_path) == 0) {
        ESP_LOGI(kCmdTag, "removed: %s", g_cmd_path);
        serial_write("OK\n");
      } else {
        char buf[300];
        snprintf(buf, sizeof(buf), "ERR:remove_failed:%s\n", g_cmd_path);
        serial_write(buf);
      }
      break;
    }
    case 'X': {
      if (!read_cmd_path("bench"))
        return;
      g_cmd_type = SerialCmdType::Bench;
      serial_write("OK\n");
      break;
    }
    case 'I': {
      if (!read_cmd_path("imgbench"))
        return;
      g_cmd_type = SerialCmdType::ImgBench;
      serial_write("OK\n");
      break;
    }
    case 'D': {
      if (!read_cmd_path("imgdecode"))
        return;
      g_cmd_type = SerialCmdType::ImgDecode;
      serial_write("OK\n");
      break;
    }
    case 'G': {
      // Flash erase+write benchmark — no path argument.
      g_cmd_type = SerialCmdType::FlashBench;
      serial_write("OK\n");
      break;
    }
    case 'F': {
      g_cmd_type = SerialCmdType::InvalidateFont;
      serial_write("FONT_INVALIDATED\n");
      break;
    }
    case 'P': {
      // Render benchmark on the currently open page (no path argument).
      g_cmd_type = SerialCmdType::RenderBench;
      serial_write("OK\n");
      break;
    }
    default:
      ESP_LOGW(kCmdTag, "unknown sub-command: 0x%02x", sub);
      serial_write("ERR:unknown_cmd\n");
      break;
  }
}
// ---------------------------------------------------------------------------
static void serial_receiver_task(void* /*arg*/) {
  uint8_t lut_pos = 0;   // progress matching kLutMagic
  uint8_t epub_pos = 0;  // progress matching kEpubMagic
  uint8_t simg_pos = 0;  // progress matching kSimgMagic
  uint8_t sdfn_pos = 0;  // progress matching kSdFntMagic
  uint8_t cmd_pos = 0;   // progress matching kCmdMagic
  uint8_t font_pos = 0;  // progress matching kFontMagic

  ESP_LOGI(kLutRxTag, "receiver ready (LUT + EPUB + SIMG + SDFN + CMD + FONT)");

  while (true) {
    uint8_t byte;
#ifdef QEMU_BUILD
    if (uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(50)) != 1)
#else
    if (usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(50)) != 1)
#endif
      continue;

    // Match LUT magic.
    if (byte == kLutMagic[lut_pos]) {
      if (++lut_pos == 4) {
        lut_pos = 0;
        epub_pos = 0;
        simg_pos = 0;
        sdfn_pos = 0;
        cmd_pos = 0;
        font_pos = 0;
        handle_lut_frame();
        continue;
      }
    } else {
      lut_pos = (byte == kLutMagic[0]) ? 1 : 0;
    }

    // Match EPUB magic.
    if (byte == kEpubMagic[epub_pos]) {
      if (++epub_pos == 4) {
        lut_pos = 0;
        epub_pos = 0;
        simg_pos = 0;
        sdfn_pos = 0;
        cmd_pos = 0;
        font_pos = 0;
        handle_epub_upload();
        continue;
      }
    } else {
      epub_pos = (byte == kEpubMagic[0]) ? 1 : 0;
    }

    // Match SIMG magic.
    if (byte == kSimgMagic[simg_pos]) {
      if (++simg_pos == 4) {
        lut_pos = 0;
        epub_pos = 0;
        simg_pos = 0;
        sdfn_pos = 0;
        cmd_pos = 0;
        font_pos = 0;
        handle_simg_upload();
        continue;
      }
    } else {
      simg_pos = (byte == kSimgMagic[0]) ? 1 : 0;
    }

    // Match SDFN magic.
    if (byte == kSdFntMagic[sdfn_pos]) {
      if (++sdfn_pos == 4) {
        lut_pos = 0;
        epub_pos = 0;
        simg_pos = 0;
        sdfn_pos = 0;
        cmd_pos = 0;
        font_pos = 0;
        handle_sdfnt_upload();
        continue;
      }
    } else {
      sdfn_pos = (byte == kSdFntMagic[0]) ? 1 : 0;
    }

    // Match CMND magic.
    if (byte == kCmdMagic[cmd_pos]) {
      if (++cmd_pos == 4) {
        lut_pos = 0;
        epub_pos = 0;
        simg_pos = 0;
        sdfn_pos = 0;
        cmd_pos = 0;
        font_pos = 0;
        handle_serial_cmd();
        continue;
      }
    } else {
      cmd_pos = (byte == kCmdMagic[0]) ? 1 : 0;
    }

    // Match FONT magic.
    if (byte == kFontMagic[font_pos]) {
      if (++font_pos == 4) {
        lut_pos = 0;
        epub_pos = 0;
        simg_pos = 0;
        sdfn_pos = 0;
        cmd_pos = 0;
        font_pos = 0;
        handle_font_upload();
        continue;
      }
    } else {
      font_pos = (byte == kFontMagic[0]) ? 1 : 0;
    }
  }
}

// Call once from app_main before the main loop.
inline void serial_start() {
#ifdef QEMU_BUILD
  // QEMU simulates UART0; route the binary protocol over it.
  const uart_config_t uart_cfg = {
      .baud_rate = 115200,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  uart_param_config(UART_NUM_0, &uart_cfg);
  uart_driver_install(UART_NUM_0, 4096, 0, 0, nullptr, 0);
  uart_vfs_dev_use_driver(0);
#else
  usb_serial_jtag_driver_config_t cfg = {
      .tx_buffer_size = 1024,
      .rx_buffer_size = 4096,
  };
  usb_serial_jtag_driver_install(&cfg);
  usb_serial_jtag_vfs_register();
#endif
  xTaskCreate(serial_receiver_task, "serial_rx", 8192, nullptr, 3, nullptr);
}
