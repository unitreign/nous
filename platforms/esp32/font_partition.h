#pragma once

// Font partition: write font data to the raw spiffs partition and memory-map
// it for zero-RAM, XIP-speed access via esp_partition_mmap().
//
// The spiffs partition (3.375MB) is used as raw storage — NOT as a SPIFFS
// filesystem. We write the FNTS font bundle directly at offset 0 and mmap it.
//
// Partition header (12 bytes):
//   [4 bytes]  "FONT" magic
//   [4 bytes]  decompressed data size (LE)
//   [4 bytes]  CRC32 of the embedded compressed bytes (LE) — used to detect
//              whether the firmware's bundled font already matches what's in
//              the partition, so we can skip re-provisioning.
//   [N bytes]  raw FNTS bundle data (uncompressed)
//
// After mmap, the returned pointer is the start of the FNTS data (after the
// 12-byte header), directly usable by BitmapFont::init() via the FNTS parser
// in main.cpp.
//
// Provisioning from embedded firmware bytes
// -----------------------------------------
// font_bundle.bin (platforms/esp32/) is a zlib-compressed FNTS bundle baked
// into firmware flash via EMBED_FILES.  Format:
//   [4 bytes LE]  uncompressed size — used to erase exactly the right blocks
//   [N bytes]     zlib-compressed FNTS bundle
// FontPartition::provision_embedded() reads the size prefix, erases the exact
// flash blocks needed upfront in one call (fast 64 KB block erases), then
// stream-decompresses via tinfl and writes.  needs_provisioning()
// does a cheap CRC check to skip the ~15s erase+write when already up-to-date.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>

#include "esp_log.h"
#include "esp_partition.h"
#include "miniz.h"

static constexpr const char* kFontPartTag = "font_part";
static constexpr uint8_t kFontPartMagic[4] = {'F', 'O', 'N', 'T'};
// Header occupies the first full 4 KB sector so that all data writes are
// sector-aligned.  Only the first 12 bytes contain meaningful data
// (magic + decompressed size + CRC32); the rest of the sector is 0xFF.
static constexpr size_t kFontPartHeaderSize = 4096;  // 1 × 4 KB sector
static constexpr size_t kFontPartHeaderBytes = 12;   // bytes actually used in sector 0

struct FontPartition {
  const uint8_t* data = nullptr;  // mmapped pointer to FNTS bundle data
  size_t size = 0;                // FNTS bundle size in bytes
  esp_partition_mmap_handle_t handle_ = 0;
  bool mapped_ = false;

  void unmap() {
    if (mapped_) {
      esp_partition_munmap(handle_);
      mapped_ = false;
    }
    data = nullptr;
    size = 0;
  }

  // Find the spiffs partition.
  static const esp_partition_t* find() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  }

  // Returns true if the partition does NOT already contain the font with the
  // given expected CRC32.  Cheap: reads 12 bytes from the partition header.
  // The expected CRC comes from the asset blob manifest (see asset_blob.h).
  static bool needs_provisioning(uint32_t expected_crc) {
    const esp_partition_t* part = find();
    if (!part)
      return true;
    uint8_t header[kFontPartHeaderBytes];
    if (esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK)
      return true;
    if (memcmp(header, kFontPartMagic, 4) != 0)
      return true;
    uint32_t stored_crc = header[8] | (header[9] << 8) | (header[10] << 16) | (header[11] << 24);
    if (stored_crc == 0)
      return true;  // invalidated or old format — treat as stale
    return stored_crc != expected_crc;
  }

  // Returns true if the given SD card file will fit in the font partition
  // (i.e. kFontPartHeaderSize + file_size <= partition size).  Call this
  // before provision_uncompressed_file to give the user a clear error message
  // without touching flash.
  static bool fits_partition(const char* path) {
    const esp_partition_t* part = find();
    if (!part)
      return false;
    FILE* f = fopen(path, "rb");
    if (!f)
      return false;
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fclose(f);
    if (file_size < 0)
      return false;
    return (size_t)(kFontPartHeaderSize + (size_t)file_size) <= part->size;
  }

  // Copy an uncompressed .mbf/.bin file from the SD card to the SPIFFS partition
  static bool provision_uncompressed_file(const char* path, uint8_t* write_buf_ext, size_t write_buf_size,
                                          std::function<void(int)> on_progress = nullptr) {
    const esp_partition_t* part = find();
    if (!part)
      return false;

    FILE* f = fopen(path, "rb");
    if (!f)
      return false;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (kFontPartHeaderSize + file_size > part->size) {
      fclose(f);
      return false;
    }

    if (on_progress)
      on_progress(0);

    size_t erase_size = (kFontPartHeaderSize + file_size + 0xFFF) & ~0xFFF;
    if (esp_partition_erase_range(part, 0, erase_size) != ESP_OK) {
      fclose(f);
      return false;
    }

    // The header (with its magic) is written only after all data is in place, so
    // an interrupted install leaves an erased, unrecognised partition rather
    // than one that looks valid but holds garbage.
    size_t write_offset = kFontPartHeaderSize;
    long remaining = file_size;

    bool own_write = false;
    if (!write_buf_ext || write_buf_size < 4096) {
      write_buf_size = 32768;
      write_buf_ext = (uint8_t*)malloc(write_buf_size);
      own_write = true;
      if (!write_buf_ext) {
        fclose(f);
        return false;
      }
    }

    bool success = true;
    while (remaining > 0) {
      size_t to_read = remaining < (long)write_buf_size ? (size_t)remaining : write_buf_size;
      size_t read_bytes = fread(write_buf_ext, 1, to_read, f);
      if (read_bytes == 0)
        break;
      if (esp_partition_write(part, write_offset, write_buf_ext, read_bytes) != ESP_OK) {
        success = false;
        break;
      }
      write_offset += read_bytes;
      remaining -= read_bytes;

      if (on_progress) {
        int pct = 5 + (int)(((file_size - remaining) * 95LL) / file_size);
        on_progress(pct);
      }
    }

    if (own_write)
      free(write_buf_ext);
    fclose(f);
    if (remaining > 0)  // short read from the SD card
      success = false;
    if (!success)
      return false;

    uint8_t header[kFontPartHeaderSize];
    memset(header, 0xFF, sizeof(header));
    memcpy(header, kFontPartMagic, 4);
    header[4] = file_size & 0xFF;
    header[5] = (file_size >> 8) & 0xFF;
    header[6] = (file_size >> 16) & 0xFF;
    header[7] = (file_size >> 24) & 0xFF;
    // CRC is 0 to distinguish it from the embedded built-in cache
    return esp_partition_write(part, 0, header, sizeof(header)) == ESP_OK;
  }

  // Stream-decompress zlib-compressed firmware font bytes and write them to
  // the spiffs partition.  Does NOT do a CRC check — call needs_provisioning()
  // first.  Returns true on success.
  //
  // The function heap-allocates ~45 KB for tinfl work + 4 KB write buffer,
  // then frees them before returning.  Call this before loading book content
  // to ensure sufficient heap headroom.
  static bool provision_embedded(const uint8_t* compressed_data, size_t compressed_size, uint32_t expected_crc,
                                 uint8_t* work_buf_ext, size_t work_buf_ext_size, uint8_t* write_buf_ext,
                                 size_t write_buf_ext_size, std::function<void(int)> on_progress = nullptr) {
    const esp_partition_t* part = find();
    if (!part) {
      ESP_LOGE(kFontPartTag, "provision: spiffs partition not found");
      return false;
    }

    // Work buffer layout: [tinfl_decompressor (11264) | dict (32768) | input (remainder)]
    static constexpr size_t kDecompSize = 11264;             // >= sizeof(tinfl_decompressor)
    static constexpr size_t kDictSize = TINFL_LZ_DICT_SIZE;  // 32768
    static constexpr size_t kEraseBlockSize =
        65536;  // 64 KB block erase (~150ms, much faster than 53× vs 835× 4KB erases)
    static constexpr size_t kMinWorkBuf = kDecompSize + kDictSize + 64;  // need at least some input buf

    // Use caller-supplied buffers if large enough; fall back to malloc.
    bool own_work = false, own_write = false;
    uint8_t* work_buf = (work_buf_ext && work_buf_ext_size >= kMinWorkBuf) ? work_buf_ext : nullptr;
    uint8_t* write_buf = (write_buf_ext && write_buf_ext_size >= 4096) ? write_buf_ext : nullptr;
    if (!work_buf) {
      static constexpr size_t kFallbackWork = kDecompSize + kDictSize + 4096;
      work_buf = static_cast<uint8_t*>(malloc(kFallbackWork));
      work_buf_ext_size = kFallbackWork;
      own_work = true;
    }
    if (!write_buf) {
      write_buf = static_cast<uint8_t*>(malloc(32768));
      write_buf_ext_size = 32768;
      own_write = true;
    }
    if (!work_buf || !write_buf) {
      if (own_work)
        free(work_buf);
      if (own_write)
        free(write_buf);
      ESP_LOGE(kFontPartTag, "provision: OOM");
      return false;
    }

    const size_t kInBufCap = work_buf_ext_size - kDecompSize - kDictSize;
    // Round down to a multiple of kDictSize (32768) so the write buffer fills
    // exactly after each tinfl dict output — ensuring flushes happen during
    // decompression rather than all at the end.  With 48 KB scratch this gives
    // 32768 bytes, which aligns perfectly with 64 KB block erases (1 erase per 2 writes).
    const size_t kWriteBufSize = (write_buf_ext_size / kDictSize) * kDictSize;

    // font_bundle.bin format: [uncompressed_size:4 LE][zlib data...]
    if (compressed_size < 4) {
      if (own_write)
        free(write_buf);
      if (own_work)
        free(work_buf);
      ESP_LOGE(kFontPartTag, "provision: font_bundle.bin too small");
      return false;
    }
    uint32_t uncompressed_size =
        compressed_data[0] | (compressed_data[1] << 8) | (compressed_data[2] << 16) | (compressed_data[3] << 24);
    // Advance past the 4-byte prefix — the rest is pure zlib.
    compressed_data += 4;
    compressed_size -= 4;

    int64_t t_start = esp_timer_get_time();
    int64_t t_erase_total = 0;
    int64_t t_write_total = 0;
    ESP_LOGI(kFontPartTag, "provision: starting (%lu uncompressed, %u compressed bytes, write_buf=%u KB)",
             (unsigned long)uncompressed_size, (unsigned)compressed_size, (unsigned)(kWriteBufSize / 1024));

    // Erase the entire region upfront in 64 KB blocks — fast (~143ms/block).
    // Total = header sector (4KB) + uncompressed data, rounded up to 64KB boundary.
    {
      uint32_t total_data = kFontPartHeaderSize + uncompressed_size;
      uint32_t erase_end = ((total_data + kEraseBlockSize - 1) / kEraseBlockSize) * kEraseBlockSize;
      uint32_t num_blocks = erase_end / kEraseBlockSize;
      int64_t te0 = esp_timer_get_time();
      for (uint32_t block = 0; block < num_blocks; ++block) {
        if (on_progress)
          on_progress(static_cast<int>(block * 30 / num_blocks));  // 0–30% during erase
        if (esp_partition_erase_range(part, block * kEraseBlockSize, kEraseBlockSize) != ESP_OK) {
          if (own_write)
            free(write_buf);
          if (own_work)
            free(work_buf);
          ESP_LOGE(kFontPartTag, "provision: upfront erase failed at block %lu", (unsigned long)block);
          return false;
        }
      }
      if (on_progress)
        on_progress(30);
      t_erase_total += esp_timer_get_time() - te0;
      ESP_LOGI(kFontPartTag, "provision: erased %lu KB in %ld ms", (unsigned long)(erase_end / 1024),
               (long)(t_erase_total / 1000));
    }

    // Set up tinfl.
    auto* decomp = reinterpret_cast<tinfl_decompressor*>(work_buf);
    uint8_t* dict = work_buf + kDecompSize;
    uint8_t* in_buf = dict + kDictSize;
    tinfl_init(decomp);

    size_t in_ofs = 0;  // bytes consumed from compressed_data
    size_t in_buf_used = 0;
    size_t in_buf_ofs = 0;
    size_t dict_ofs = 0;

    size_t write_buf_used = 0;
    // Data starts at kFontPartHeaderSize (4096) — everything already erased.
    uint32_t flash_offset = kFontPartHeaderSize;
    uint32_t decompressed_size = 0;
    bool ok = true;

    for (;;) {
      // Refill input buffer from embedded compressed data.
      if (in_buf_ofs >= in_buf_used && in_ofs < compressed_size) {
        size_t to_copy = std::min(kInBufCap, compressed_size - in_ofs);
        memcpy(in_buf, compressed_data + in_ofs, to_copy);
        in_buf_used = to_copy;
        in_buf_ofs = 0;
        in_ofs += to_copy;
        if (on_progress)
          on_progress(30 + static_cast<int>(in_ofs * 70 / compressed_size));  // 30–100% during write
      }

      size_t in_bytes = in_buf_used - in_buf_ofs;
      size_t out_bytes = kDictSize - dict_ofs;

      mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
      if (in_ofs < compressed_size)
        flags |= TINFL_FLAG_HAS_MORE_INPUT;

      tinfl_status status =
          tinfl_decompress(decomp, in_buf + in_buf_ofs, &in_bytes, dict, dict + dict_ofs, &out_bytes, flags);
      in_buf_ofs += in_bytes;

      // Copy decompressed output into write_buf, flushing when full.
      if (out_bytes > 0) {
        const uint8_t* out_ptr = dict + dict_ofs;
        size_t out_remaining = out_bytes;
        while (out_remaining > 0) {
          size_t to_copy = std::min(out_remaining, kWriteBufSize - write_buf_used);
          memcpy(write_buf + write_buf_used, out_ptr, to_copy);
          write_buf_used += to_copy;
          out_ptr += to_copy;
          out_remaining -= to_copy;
          if (write_buf_used == kWriteBufSize) {
            int64_t tw0 = esp_timer_get_time();
            if (esp_partition_write(part, flash_offset, write_buf, kWriteBufSize) != ESP_OK) {
              ESP_LOGE(kFontPartTag, "provision: write failed at offset %lu", (unsigned long)flash_offset);
              ok = false;
              break;
            }
            t_write_total += esp_timer_get_time() - tw0;
            flash_offset += kWriteBufSize;
            decompressed_size += kWriteBufSize;
            write_buf_used = 0;
          }
        }
        dict_ofs = (dict_ofs + out_bytes) & (kDictSize - 1);
      }

      if (!ok || status == TINFL_STATUS_DONE)
        break;
      if (status < TINFL_STATUS_DONE) {
        ESP_LOGE(kFontPartTag, "provision: decompression error (status=%d)", (int)status);
        ok = false;
        break;
      }
    }

    // Flush any remaining bytes (pad to 4-byte alignment). No erase needed — done upfront.
    if (ok && write_buf_used > 0) {
      size_t padded = (write_buf_used + 3) & ~3u;
      memset(write_buf + write_buf_used, 0xFF, padded - write_buf_used);
      int64_t tw0 = esp_timer_get_time();
      if (esp_partition_write(part, flash_offset, write_buf, padded) != ESP_OK) {
        ESP_LOGE(kFontPartTag, "provision: final write failed");
        ok = false;
      } else {
        t_write_total += esp_timer_get_time() - tw0;
        decompressed_size += write_buf_used;
      }
    }

    if (own_write)
      free(write_buf);
    if (own_work)
      free(work_buf);

    if (!ok)
      return false;

    // Write the 12-byte header into sector 0 (already erased above).
    {
      uint32_t embedded_crc = expected_crc;
      uint8_t hdr[kFontPartHeaderBytes] = {};
      memcpy(hdr, kFontPartMagic, 4);
      hdr[4] = decompressed_size & 0xFF;
      hdr[5] = (decompressed_size >> 8) & 0xFF;
      hdr[6] = (decompressed_size >> 16) & 0xFF;
      hdr[7] = (decompressed_size >> 24) & 0xFF;
      hdr[8] = embedded_crc & 0xFF;
      hdr[9] = (embedded_crc >> 8) & 0xFF;
      hdr[10] = (embedded_crc >> 16) & 0xFF;
      hdr[11] = (embedded_crc >> 24) & 0xFF;
      esp_partition_write(part, 0, hdr, sizeof(hdr));

      long t_total_ms = (long)((esp_timer_get_time() - t_start) / 1000);
      long t_erase_ms = (long)(t_erase_total / 1000);
      long t_write_ms = (long)(t_write_total / 1000);
      ESP_LOGI(kFontPartTag, "provisioned %lu bytes in %ld ms (erase=%ld ms, write=%ld ms, other=%ld ms, CRC=0x%08lx)",
               (unsigned long)decompressed_size, t_total_ms, t_erase_ms, t_write_ms,
               t_total_ms - t_erase_ms - t_write_ms, (unsigned long)embedded_crc);
    }
    return true;
  }

  // Zeros the CRC field in the partition header so needs_provisioning() returns
  // true, triggering re-provisioning on the next book open.  Returns true on success.
  static bool invalidate() {
    const esp_partition_t* part = find();
    if (!part)
      return false;
    uint8_t hdr[kFontPartHeaderBytes];
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK)
      return false;
    hdr[8] = hdr[9] = hdr[10] = hdr[11] = 0;
    if (esp_partition_erase_range(part, 0, 4096) != ESP_OK)
      return false;
    if (esp_partition_write(part, 0, hdr, sizeof(hdr)) != ESP_OK)
      return false;
    ESP_LOGI(kFontPartTag, "font partition invalidated — will re-provision on next book open");
    return true;
  }

  // Write raw (uncompressed) font data to the partition.  Used by the serial
  // upload path.  The header stores CRC=0 so needs_provisioning() always
  // returns true afterwards, ensuring the next book open re-provisions from
  // the embedded firmware font.
  // Returns true on success.
  static bool write(const uint8_t* font_data, size_t font_size) {
    const esp_partition_t* part = find();
    if (!part) {
      ESP_LOGE(kFontPartTag, "spiffs partition not found");
      return false;
    }
    size_t total = kFontPartHeaderSize + font_size;
    if (total > part->size) {
      ESP_LOGE(kFontPartTag, "font too large: %u > partition %lu", (unsigned)total, (unsigned long)part->size);
      return false;
    }

    // Erase the sectors we need (rounded up to 4KB).
    size_t erase_size = (total + 0xFFF) & ~0xFFF;
    esp_err_t err = esp_partition_erase_range(part, 0, erase_size);
    if (err != ESP_OK) {
      ESP_LOGE(kFontPartTag, "erase failed: %s", esp_err_to_name(err));
      return false;
    }

    // Write header: magic + size + CRC (CRC=0: marks as "manually uploaded",
    // needs_provisioning() will return true so it gets overwritten on next book open).
    uint8_t header[kFontPartHeaderSize] = {};
    memcpy(header, kFontPartMagic, 4);
    header[4] = font_size & 0xFF;
    header[5] = (font_size >> 8) & 0xFF;
    header[6] = (font_size >> 16) & 0xFF;
    header[7] = (font_size >> 24) & 0xFF;
    // header[8..11] = 0 (CRC), so next boot will re-provision from firmware
    err = esp_partition_write(part, 0, header, kFontPartHeaderSize);
    if (err != ESP_OK) {
      ESP_LOGE(kFontPartTag, "header write failed: %s", esp_err_to_name(err));
      return false;
    }

    // Write font data in 4KB chunks.
    size_t offset = kFontPartHeaderSize;
    size_t remaining = font_size;
    const uint8_t* src = font_data;
    while (remaining > 0) {
      size_t chunk = remaining > 4096 ? 4096 : remaining;
      err = esp_partition_write(part, offset, src, chunk);
      if (err != ESP_OK) {
        ESP_LOGE(kFontPartTag, "data write failed at offset %u: %s", (unsigned)offset, esp_err_to_name(err));
        return false;
      }
      offset += chunk;
      src += chunk;
      remaining -= chunk;
    }

    ESP_LOGI(kFontPartTag, "wrote %u bytes to spiffs partition", (unsigned)font_size);
    return true;
  }

  // Memory-map the font from the partition. Returns true if a valid font
  // was found and mapped. After success, data/size are set.
  bool mmap() {
    const esp_partition_t* part = find();
    if (!part) {
      ESP_LOGW(kFontPartTag, "spiffs partition not found");
      return false;
    }

    // Read header to check magic and get size (only need the first 12 bytes).
    uint8_t header[kFontPartHeaderBytes];
    if (esp_partition_read(part, 0, header, kFontPartHeaderBytes) != ESP_OK) {
      ESP_LOGW(kFontPartTag, "failed to read header");
      return false;
    }
    if (memcmp(header, kFontPartMagic, 4) != 0) {
      ESP_LOGI(kFontPartTag, "no font in partition (magic mismatch)");
      return false;
    }
    uint32_t font_size = header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);
    if (font_size == 0 || kFontPartHeaderSize + font_size > part->size) {
      ESP_LOGW(kFontPartTag, "invalid font size: %lu", (unsigned long)font_size);
      return false;
    }

    // Release the previous mapping first; each map holds MMU pages from a small
    // fixed pool. Callers reload their font set right after this returns.
    unmap();

    // Memory-map the font data region.
    esp_partition_mmap_handle_t handle;
    const void* mapped = nullptr;
    esp_err_t err = esp_partition_mmap(part, kFontPartHeaderSize, font_size, ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (err != ESP_OK) {
      ESP_LOGE(kFontPartTag, "mmap failed: %s", esp_err_to_name(err));
      return false;
    }

    handle_ = handle;
    mapped_ = true;
    data = static_cast<const uint8_t*>(mapped);
    size = font_size;
    ESP_LOGI(kFontPartTag, "mmapped font: %lu bytes at %p", (unsigned long)font_size, mapped);
    return true;
  }

  // -------------------------------------------------------------------------
  // Flash erase+write benchmark.
  // Tests a matrix of erase chunk sizes × write chunk sizes over a fixed
  // 512 KB region at the START of the partition (offset 0).
  //
  // For each combination, the function:
  //   1. Erases the 512 KB region in chunks of `erase_chunk` bytes.
  //   2. Writes the 512 KB region in chunks of `write_chunk` bytes.
  //   3. Logs the individual erase + write times in milliseconds.
  //
  // The test region always starts at offset 0 in the partition, so it
  // DESTROYS any font data currently stored there.  Call invalidate() or
  // re-provision after running this benchmark.
  //
  // `scratch` must point to at least `max_chunk` bytes of writable memory.
  // max_chunk is the largest write chunk used (65536 bytes recommended).
  //
  // Typical output (one line per combination):
  //   FLASH_BENCH erase=65536 write=32768 erase_ms=8234 write_ms=17100
  // -------------------------------------------------------------------------
  static void bench_flash(uint8_t* scratch, size_t scratch_size) {
    const esp_partition_t* part = find();
    if (!part) {
      ESP_LOGE(kFontPartTag, "bench_flash: spiffs partition not found");
      return;
    }

    static constexpr size_t kTestBytes = 512 * 1024;  // 512 KB test region
    static constexpr size_t kEraseSizes[] = {4096, 32768, 65536};
    static constexpr size_t kWriteSizes[] = {4096, 16384, 32768, 65536};

    ESP_LOGI(kFontPartTag, "=== FLASH BENCH START (region=%u KB) ===", (unsigned)(kTestBytes / 1024));

    for (size_t erase_chunk : kEraseSizes) {
      for (size_t write_chunk : kWriteSizes) {
        if (write_chunk > scratch_size) {
          ESP_LOGW(kFontPartTag, "FLASH_BENCH erase=%u write=%u  SKIP (scratch too small)", (unsigned)erase_chunk,
                   (unsigned)write_chunk);
          continue;
        }
        // Fill scratch with a known pattern.
        memset(scratch, 0xA5, write_chunk);

        // --- Erase phase ---
        int64_t te0 = esp_timer_get_time();
        bool ok = true;
        for (size_t offset = 0; offset < kTestBytes; offset += erase_chunk) {
          size_t chunk = std::min(erase_chunk, kTestBytes - offset);
          // Round up to erase_chunk to stay aligned.
          if (chunk < erase_chunk)
            chunk = erase_chunk;
          if (offset + chunk > part->size) {
            ok = false;
            ESP_LOGE(kFontPartTag, "bench_flash: region exceeds partition size");
            break;
          }
          if (esp_partition_erase_range(part, (uint32_t)offset, (uint32_t)chunk) != ESP_OK) {
            ok = false;
            ESP_LOGE(kFontPartTag, "bench_flash: erase failed at offset %u", (unsigned)offset);
            break;
          }
        }
        long erase_ms = (long)((esp_timer_get_time() - te0) / 1000);
        if (!ok)
          continue;

        // --- Write phase ---
        int64_t tw0 = esp_timer_get_time();
        for (size_t offset = 0; offset < kTestBytes; offset += write_chunk) {
          size_t chunk = std::min(write_chunk, kTestBytes - offset);
          if (esp_partition_write(part, (uint32_t)offset, scratch, chunk) != ESP_OK) {
            ESP_LOGE(kFontPartTag, "bench_flash: write failed at offset %u", (unsigned)offset);
            ok = false;
            break;
          }
        }
        long write_ms = (long)((esp_timer_get_time() - tw0) / 1000);
        if (!ok)
          continue;

        ESP_LOGI(kFontPartTag, "FLASH_BENCH erase_chunk=%u write_chunk=%u erase_ms=%ld write_ms=%ld total_ms=%ld",
                 (unsigned)erase_chunk, (unsigned)write_chunk, erase_ms, write_ms, erase_ms + write_ms);
      }
    }

    // Erase sector 0 to destroy the header — forces re-provisioning on next boot.
    esp_partition_erase_range(part, 0, 4096);
    ESP_LOGI(kFontPartTag, "=== FLASH BENCH DONE (partition header erased — will re-provision on next book open) ===");
  }
};
