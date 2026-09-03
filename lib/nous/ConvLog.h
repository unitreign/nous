#pragma once

// Lightweight conversion logger — appends to convert_log.txt on the SD card.
// Each call opens, writes one line, closes (no FD held between calls).
// Compiles away entirely on non-ESP builds.

#ifdef ESP_PLATFORM
#include <cstdarg>
#include <cstdio>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Controlled by Settings > System > Debug Log. Default off.
extern bool g_conv_log_enabled;

static constexpr const char* kConvLogPath = "/sdcard/.microreader/convert_log.txt";

inline void clog_write(const char* fmt, ...) {
  if (!g_conv_log_enabled) return;
  FILE* f = fopen(kConvLogPath, "a");
  if (!f) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

// Logs a line + current heap/stack snapshot on the next line.
inline void clog_heap(const char* tag) {
  if (!g_conv_log_enabled) return;
  clog_write("[%s] heap_free=%lu largest=%lu stack_hwm=%lu", tag,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long)uxTaskGetStackHighWaterMark(NULL));
}

#define CLOG(...) clog_write(__VA_ARGS__)
#define CLOG_HEAP(tag) clog_heap(tag)

#else
#define CLOG(...) (void)0
#define CLOG_HEAP(tag) (void)0
#endif
