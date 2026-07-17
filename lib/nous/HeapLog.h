#pragma once

// HEAP_LOG(tag) — logs free heap and largest free block on ESP32, no-op on desktop.
// MR_LOGI(tag, fmt, ...) — printf-style info log on both platforms.
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#define HEAP_LOG(tag)                                                                       \
  ESP_LOGI("mem", "%s: free=%lu largest=%lu", tag, (unsigned long)esp_get_free_heap_size(), \
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))
#define MR_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#else
#include <cstdio>
#define HEAP_LOG(tag) ((void)0)
#define MR_LOGI(tag, fmt, ...) (printf("[%s] " fmt "\n", tag, ##__VA_ARGS__))
#endif
