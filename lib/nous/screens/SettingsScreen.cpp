#include "SettingsScreen.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "../Application.h"
#include "../content/BookIndex.h"
#include "../version.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "miniz.h"
#else
#include <filesystem>
#endif

#include "../resources/spiffs_image_data.h"

namespace microreader {

static std::string get_menu_nav_label(bool inverted) {
  return std::string("Menu Nav: ") + (inverted ? "Right=Down" : "Right=Up");
}

static std::string get_bottom_paging_label(bool inverted) {
  return std::string("Bottom Paging: ") + (inverted ? "Right=Prev" : "Right=Next");
}

static std::string get_side_paging_label(bool inverted) {
  return std::string("Side Paging: ") + (inverted ? "Top=Prev" : "Top=Next");
}

static std::string get_sort_order_label(BookSortOrder order) {
  return std::string("Sort: ") + (order == BookSortOrder::LastOpened ? "Last Opened" : "Alphabetical");
}

static std::string get_list_format_label(BookListFormat fmt) {
  if (fmt == BookListFormat::TitleOnly)
    return "Book List: Title";
  if (fmt == BookListFormat::Filename)
    return "Book List: Filename";
  return "Book List: Title & Author";
}

static std::string get_rotate_menu_label(uint8_t v) {
  return std::string("Menu: ") + rotation_label(v);
}

static std::string get_rotate_reader_label(uint8_t v) {
  return std::string("Reader: ") + rotation_label(v);
}

static std::string get_menu_font_label(int size) {
  if (size == 1) return "Menu Size: Medium";
  if (size == 2) return "Menu Size: Large";
  if (size == 3) return "Menu Size: X-Large";
  return "Menu Size: Small";
}

static std::string get_sleep_image_label(const std::string& path) {
  std::string label = "Sleep Image: ";
  if (path.rfind("embedded:", 0) == 0) {
    label += "nous";
  } else {
    const char* p = path.rfind("bmp:", 0) == 0 ? path.c_str() + 4 : path.c_str();
    const char* slash = nullptr;
    for (const char* c = p; *c; ++c)
      if (*c == '/' || *c == '\\')
        slash = c;
    label += (slash ? slash + 1 : p);
  }
  return label;
}

static std::string get_nav_arrows_label(bool shown) {
  return std::string("Nav Arrows: ") + (shown ? "Show" : "Hide");
}

static std::string get_reader_images_label(bool enabled) {
  return std::string("Book Images: ") + (enabled ? "On" : "Off");
}

static std::string get_conv_indicator_label(bool shown) {
  return std::string("Converted Mark: ") + (shown ? "On" : "Off");
}

static std::string get_battery_display_label(uint8_t mode) {
  if (mode == 1) return "Battery: Number";
  if (mode == 2) return "Battery: Icon & Number";
  return "Battery: Icon";
}

static std::string get_theme_label(uint8_t theme) {
  if (theme == 0) return "Theme: Chronicle";
  if (theme == 2) return "Theme: Stele";
  if (theme == 3) return "Theme: Codex";
  if (theme == 4) return "Theme: Lyra";
  return "Theme: Minimal";
}

static std::string get_sleep_timeout_label(uint8_t min) {
  if (min == 0) return "Auto Sleep: Off";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "Auto Sleep: %d min", static_cast<int>(min));
  return buf;
}

static std::string get_list_align_label(uint8_t align) {
  if (align == 1) return "List Align: Left";
  if (align == 2) return "List Align: Right";
  return "List Align: Center";
}

static std::string get_font_label(const std::string& font_path) {
  std::string label = "Font: ";
  if (font_path == "Bookerly" || font_path == "Alegreya") {
    label += font_path;
  } else {
    const char* p = font_path.c_str();
    const char* slash = nullptr;
    for (const char* c = p; *c; ++c)
      if (*c == '/' || *c == '\\')
        slash = c;
    label += std::string(slash ? slash + 1 : p);
  }
  return label;
}

void SettingsScreen::on_start() {
  title_ = "Settings";
  subtitle_ = MICROREADER_VERSION;

  sd_fonts_.clear();
  sd_fonts_.push_back("Bookerly");
  sd_fonts_.push_back("Alegreya");
  font_sel_idx_ = 0;
#ifdef ESP_PLATFORM
  DIR* d = opendir("/sdcard/fonts");
  if (d) {
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
      if (ent->d_name[0] == '.')
        continue;
      const char* ext = std::strrchr(ent->d_name, '.');
      if (ext && strcmp(ext, ".mfb") == 0) {
        sd_fonts_.push_back(std::string("/sdcard/fonts/") + ent->d_name);
      }
    }
    closedir(d);
  }
#else
  namespace fs = std::filesystem;
  try {
    for (const auto& entry : fs::directory_iterator("sd/fonts")) {
      std::string ext = entry.path().extension().string();
      if (ext == ".mfb") {
        sd_fonts_.push_back(entry.path().string());
      }
    }
  } catch (...) {}
#endif

  if (app_) {
    const std::string& current = app_->custom_font_path();
    for (size_t i = 0; i < sd_fonts_.size(); ++i) {
      if (sd_fonts_[i] == current) {
        font_sel_idx_ = static_cast<int>(i);
        break;
      }
    }
  }

  // Sleep image list: "nous" (embedded:0) is always first; SD images follow.
  sleep_images_.clear();
  sleep_images_.push_back("embedded:0");  // nous — always present
  sleep_image_sel_idx_ = 0;
#ifdef ESP_PLATFORM
  {
    DIR* sd = opendir("/sdcard/.sleep");
    if (sd) {
      struct dirent* ent;
      while ((ent = readdir(sd)) != nullptr) {
        if (ent->d_name[0] == '.')
          continue;
        const char* ext = std::strrchr(ent->d_name, '.');
        if (!ext) continue;
        if (strcmp(ext, ".mgr") == 0)
          sleep_images_.push_back(std::string("/sdcard/.sleep/") + ent->d_name);
        else if (strcmp(ext, ".bmp") == 0)
          sleep_images_.push_back(std::string("bmp:/sdcard/.sleep/") + ent->d_name);
      }
      closedir(sd);
    }
  }
#else
  try {
    for (const auto& entry : fs::directory_iterator("sd/.sleep")) {
      const auto& p = entry.path();
      if (p.extension() == ".mgr")
        sleep_images_.push_back(p.string());
      else if (p.extension() == ".bmp")
        sleep_images_.push_back("bmp:" + p.string());
    }
  } catch (...) {}
#endif
  if (app_) {
    const std::string& current = app_->sleep_image_path();
    for (size_t i = 0; i < sleep_images_.size(); ++i) {
      if (sleep_images_[i] == current) {
        sleep_image_sel_idx_ = static_cast<int>(i);
        break;
      }
    }
  }

  // --- Appearance ---
  add_separator("APPEARANCE");
  idx_rotate_display_ = count();
  add_item(get_rotate_menu_label(app_ ? app_->rotate_display() : 0));

  idx_reader_rotate_display_ = count();
  add_item(get_rotate_reader_label(app_ ? app_->rotate_reader() : 0));

  idx_theme_ = count();
  add_item(get_theme_label(app_ ? app_->menu_theme() : 0));

  idx_menu_font_ = count();
  add_item(get_menu_font_label(app_ ? app_->menu_font_size() : 0));

  idx_list_format_ = count();
  if (app_) {
    add_item(get_list_format_label(app_->main_menu() ? app_->main_menu()->list_format() : BookListFormat::TitleOnly));
  } else {
    add_item("List: Title");
  }

  idx_sort_order_ = count();
  add_item(get_sort_order_label(app_ ? app_->sort_order() : BookSortOrder::Alphabetical));

  idx_font_ = count();
  add_item(get_font_label(sd_fonts_[font_sel_idx_]));

  idx_sleep_image_ = count();
  add_item(get_sleep_image_label(sleep_images_[sleep_image_sel_idx_]));

  idx_nav_arrows_ = count();
  add_item(get_nav_arrows_label(app_ ? app_->show_nav_arrows() : true));

  idx_reader_images_ = count();
  add_item(get_reader_images_label(app_ ? app_->show_reader_images() : true));

  idx_battery_display_ = count();
  add_item(get_battery_display_label(app_ ? app_->battery_display() : 0));

  idx_conv_indicator_ = count();
  add_item(get_conv_indicator_label(app_ ? app_->show_converted_indicator() : false));

  idx_list_align_ = count();
  add_item(get_list_align_label(app_ ? app_->list_align() : 0));

  idx_sleep_timeout_ = count();
  add_item(get_sleep_timeout_label(app_ ? app_->sleep_timeout_min() : 10));

  add_separator("CONTROLS");

  // --- Controls ---
  idx_invert_side_ = count();
  add_item(get_side_paging_label(app_->invert_side_buttons()));

  idx_invert_bottom_paging_ = count();
  add_item(get_bottom_paging_label(app_->invert_bottom_paging()));

  idx_invert_menu_ = count();
  add_item(get_menu_nav_label(app_->invert_menu_buttons()));

  add_separator("SYSTEM");

  if (data_dir_) {
    idx_clear_cache_ = count();
    add_item("Clear Cache");

    idx_rebuild_index_ = count();
    add_item("Rebuild Book Index");
  }

#ifdef ESP_PLATFORM
  if (app_ && app_->has_invalidate_font_fn()) {
    idx_invalidate_font_ = count();
    add_item("Invalidate Font");
  }

  idx_spiffs_ = count();
  add_item("Rebuild SPIFFS");

  {
    auto running = esp_ota_get_running_partition();
    auto next = esp_ota_get_next_update_partition(running);
    if (next) {
      esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
      esp_ota_get_state_partition(next, &state);
      ESP_LOGI("OTA", "on_start: next=%s state=%d", next->label, (int)state);
      if (state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_NEW || state == ESP_OTA_IMG_PENDING_VERIFY ||
          state == ESP_OTA_IMG_UNDEFINED) {
        idx_switch_ota_ = count();
        char label[24];
        std::snprintf(label, sizeof(label), "Switch to %s", next->label);
        add_item(label);
      }
    }
  }
#endif

  // --- Demos ---
#ifdef MICROREADER_ENABLE_DEMOS
  add_separator();
  idx_bouncing_ball_ = count();
  add_item("Bouncing Ball");

  idx_grayscale_demo_ = count();
  add_item("Grayscale Demo");
#endif

  add_separator();
  idx_convert_all_ = count();
  add_item("Convert All Books");
}

int SettingsScreen::get_visible_count_(int H, int scroll_off) const {
  const int list_top = 16 + (header_font_.valid() ? header_font_.y_advance() : ui_font_.y_advance()) + 7;
  const int available_h = H - list_top;
  int h = 0, cnt = 0, n = count();
  for (int i = scroll_off; i < n; ++i) {
    const int item_h = is_separator(i)
        ? (8 + (!get_item_label(i).empty() && section_font_.valid() ? section_font_.y_advance() : 0) + 4)
        : kRowH;
    if (h + item_h > available_h) break;
    h += item_h;
    cnt++;
  }
  return cnt;
}

std::string_view SettingsScreen::get_item_subtitle(int index) const {
  std::string_view label = ListMenuScreen::get_item_label(index);
  const auto pos = label.find(": ");
  if (pos == std::string_view::npos) return {};
  subtitle_buf_ = label.substr(pos + 2);
  return subtitle_buf_;
}

void SettingsScreen::on_select(int index) {
  if (index == idx_theme_) {
    if (app_) {
      uint8_t v = static_cast<uint8_t>((app_->menu_theme() + 1) % 5);
      app_->set_menu_theme(v);
      set_item_label(idx_theme_, get_theme_label(v));
    }
    return;
  }
  if (index == idx_nav_arrows_) {
    if (app_) {
      bool v = !app_->show_nav_arrows();
      app_->set_show_nav_arrows(v);
      set_item_label(idx_nav_arrows_, get_nav_arrows_label(v));
    }
    return;
  }
  if (index == idx_reader_images_) {
    if (app_) {
      bool v = !app_->show_reader_images();
      app_->set_show_reader_images(v);
      set_item_label(idx_reader_images_, get_reader_images_label(v));
    }
    return;
  }
  if (index == idx_battery_display_) {
    if (app_) {
      uint8_t v = static_cast<uint8_t>((app_->battery_display() + 1) % 3);
      app_->set_battery_display(v);
      set_item_label(idx_battery_display_, get_battery_display_label(v));
    }
    return;
  }
  if (index == idx_conv_indicator_) {
    if (app_) {
      bool v = !app_->show_converted_indicator();
      app_->set_show_converted_indicator(v);
      set_item_label(idx_conv_indicator_, get_conv_indicator_label(v));
    }
    return;
  }
  if (index == idx_list_align_) {
    if (app_) {
      uint8_t v = static_cast<uint8_t>((app_->list_align() + 1) % 3);
      app_->set_list_align(v);
      set_item_label(idx_list_align_, get_list_align_label(v));
    }
    return;
  }
  if (index == idx_sleep_timeout_) {
    if (app_) {
      static constexpr uint8_t kTimeouts[] = {1, 3, 5, 10, 20, 30, 0};
      uint8_t cur = app_->sleep_timeout_min();
      uint8_t next = kTimeouts[0];
      for (size_t i = 0; i < sizeof(kTimeouts); ++i) {
        if (kTimeouts[i] == cur) {
          next = kTimeouts[(i + 1) % sizeof(kTimeouts)];
          break;
        }
      }
      app_->set_sleep_timeout_min(next);
      set_item_label(idx_sleep_timeout_, get_sleep_timeout_label(next));
    }
    return;
  }
  if (index == idx_convert_all_) {
    if (app_)
      app_->push_screen(ScreenId::ConvertAll);
    return;
  }
#ifdef MICROREADER_ENABLE_DEMOS
  if (index == idx_bouncing_ball_) {
    app_->push_screen(ScreenId::BouncingBall);
    return;
  }
  if (index == idx_grayscale_demo_) {
    app_->push_screen(ScreenId::GrayscaleDemo);
    return;
  }
#endif
  if (index == idx_clear_cache_) {
    clear_cache_();
    toast_original_label_ = get_item_label(idx_clear_cache_);
    toast_idx_ = idx_clear_cache_;
    toast_frames_ = 15;
    set_item_label(idx_clear_cache_, "Cache cleared!");
    return;
  }
  if (index == idx_rebuild_index_) {
    if (app_->main_menu() && app_->main_menu()->has_books_dir() && app_->data_dir_) {
      std::string root_dir = app_->main_menu()->books_dir();
      std::string index_path = std::string(app_->data_dir_) + "/book_index.dat";

      buf_->sync_bw_ram();
      BookIndex::instance().load(index_path);
      BookIndex::instance().build_index(root_dir, *buf_);
      BookIndex::instance().save(index_path);
      buf_->reset_after_scratch(true);
      app_->pop_screen();  // go back to main menu
    }
    return;
  }
  if (index == idx_sort_order_) {
    if (app_) {
      BookSortOrder order = (app_->sort_order() == BookSortOrder::Alphabetical) ? BookSortOrder::LastOpened : BookSortOrder::Alphabetical;
      app_->set_sort_order(order);
      set_item_label(idx_sort_order_, get_sort_order_label(order));
    }
    return;
  }
  if (index == idx_list_format_) {
    if (app_->main_menu()) {
      auto fmt = app_->main_menu()->list_format();
      if (fmt == BookListFormat::TitleAndAuthor) {
        fmt = BookListFormat::TitleOnly;
      } else if (fmt == BookListFormat::TitleOnly) {
        fmt = BookListFormat::Filename;
      } else {
        fmt = BookListFormat::TitleAndAuthor;
      }
      app_->main_menu()->set_list_format(fmt);
      set_item_label(idx_list_format_, get_list_format_label(fmt));
    }
    app_->save_settings_();
    return;
  }
  if (index == idx_invert_menu_) {
    if (app_) {
      bool v = !app_->invert_menu_buttons();
      app_->set_invert_menu_buttons(v);
      set_item_label(idx_invert_menu_, get_menu_nav_label(v));
    }
    return;
  }
  if (index == idx_invert_bottom_paging_) {
    if (app_) {
      bool v = !app_->invert_bottom_paging();
      app_->set_invert_bottom_paging(v);
      set_item_label(idx_invert_bottom_paging_, get_bottom_paging_label(v));
    }
    return;
  }
  if (index == idx_invert_side_) {
    if (app_) {
      bool v = !app_->invert_side_buttons();
      app_->set_invert_side_buttons(v);
      set_item_label(idx_invert_side_, get_side_paging_label(v));
    }
    return;
  }
  if (index == idx_rotate_display_) {
    if (app_ && buf_) {
      uint8_t v = static_cast<uint8_t>((app_->rotate_display() + 1) % 4);
      app_->set_rotate_display(v);
      set_item_label(idx_rotate_display_, get_rotate_menu_label(v));
      buf_->set_rotation(rotation_from_setting(v));
    }
    return;
  }
  if (index == idx_reader_rotate_display_) {
    if (app_) {
      uint8_t v = static_cast<uint8_t>((app_->rotate_reader() + 1) % 4);
      app_->set_rotate_reader(v);
      set_item_label(idx_reader_rotate_display_, get_rotate_reader_label(v));
    }
    return;
  }
  if (index == idx_menu_font_) {
    if (app_) {
      int v = (app_->menu_font_size() + 1) % 4;
      app_->set_menu_font_size(v);
      restart();  // rebuilds items with the new font size immediately
    }
    return;
  }
  if (index == idx_font_) {
    if (app_ && !sd_fonts_.empty()) {
      font_sel_idx_ = (font_sel_idx_ + 1) % sd_fonts_.size();
      app_->set_custom_font_path(sd_fonts_[font_sel_idx_]);
      set_item_label(idx_font_, get_font_label(sd_fonts_[font_sel_idx_]));
    }
    return;
  }
  if (index == idx_sleep_image_) {
    if (app_ && !sleep_images_.empty()) {
      sleep_image_sel_idx_ = (sleep_image_sel_idx_ + 1) % static_cast<int>(sleep_images_.size());
      app_->set_sleep_image_path(sleep_images_[sleep_image_sel_idx_]);
      set_item_label(idx_sleep_image_, get_sleep_image_label(sleep_images_[sleep_image_sel_idx_]));
    }
    return;
  }
#ifdef ESP_PLATFORM
  if (index == idx_switch_ota_) {
    switch_ota_partition_();
    return;
  }
  if (index == idx_invalidate_font_) {
    if (app_) {
      app_->set_installed_font_path("");
      app_->invalidate_font();
      toast_original_label_ = get_item_label(idx_invalidate_font_);
      toast_idx_ = idx_invalidate_font_;
      toast_frames_ = 15;
      set_item_label(idx_invalidate_font_, "Font invalidated!");
    }
    return;
  }
  if (index == idx_spiffs_) {
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");

    if (part) {
      buf_->sync_bw_ram();
      buf_->show_loading("Erasing...", 0);

      static constexpr size_t kDictSize = TINFL_LZ_DICT_SIZE;
      static constexpr size_t kDecompSize = 11264;
      static constexpr size_t kWriteSize = 4096;
      uint8_t* work = static_cast<uint8_t*>(malloc(kDecompSize + kDictSize + kWriteSize));
      if (work) {
        esp_partition_erase_range(part, 0, part->size);

        if (buf_)
          buf_->show_loading("Writing...", 50);

        auto* decomp = reinterpret_cast<tinfl_decompressor*>(work);
        uint8_t* dict = work + kDecompSize;
        uint8_t* wbuf = work + kDecompSize + kDictSize;
        tinfl_init(decomp);
        const uint8_t* in_ptr = kSpiffsImage;
        size_t in_left = kSpiffsImageSize;
        size_t flash_offset = 0;
        size_t dict_ofs = 0;
        tinfl_status status = TINFL_STATUS_HAS_MORE_OUTPUT;
        while (status == TINFL_STATUS_HAS_MORE_OUTPUT || status == TINFL_STATUS_NEEDS_MORE_INPUT) {
          size_t in_sz = in_left;
          size_t out_sz = kDictSize - dict_ofs;
          mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
          if (in_left > in_sz)
            flags |= TINFL_FLAG_HAS_MORE_INPUT;
          status = tinfl_decompress(decomp, in_ptr, &in_sz, dict, dict + dict_ofs, &out_sz, flags);
          in_ptr += in_sz;
          in_left -= in_sz;
          size_t produced = out_sz;
          size_t write_ofs = 0;
          while (write_ofs < produced) {
            size_t chunk = produced - write_ofs;
            if (chunk > kWriteSize)
              chunk = kWriteSize;
            memcpy(wbuf, dict + dict_ofs + write_ofs, chunk);
            esp_partition_write(part, flash_offset, wbuf, chunk);
            flash_offset += chunk;
            write_ofs += chunk;
          }
          dict_ofs = (dict_ofs + produced) & (kDictSize - 1);
          if (status <= TINFL_STATUS_DONE)
            break;
        }
        free(work);

        buf_->show_loading("Done!", 100);
      }
    }
    esp_restart();
  }
#endif
  return;
}

#ifdef ESP_PLATFORM
// Fallback: write the otadata partition directly, bypassing esp_image_verify.
// Mirrors what tools/switch_partition.py does from the host side.
// Use only when esp_ota_set_boot_partition refuses an image we know is
// good (e.g. foreign firmware whose seg0 is too large to mmap from a
// running app on ESP32-C3).
static esp_err_t force_switch_via_otadata_(const esp_partition_t* next) {
  if (!next)
    return ESP_ERR_INVALID_ARG;
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata) {
    ESP_LOGE("OTA", "otadata partition not found");
    return ESP_ERR_NOT_FOUND;
  }

  // ota_seq: 1 -> ota_0 (app0), 2 -> ota_1 (app1)
  uint32_t seq = (next->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) ? 1u : 2u;
  uint8_t entry[32];
  std::memset(entry, 0xFF, sizeof(entry));
  std::memcpy(entry, &seq, 4);
  uint32_t crc = esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&seq), 4);
  std::memcpy(entry + 28, &crc, 4);

  esp_err_t err = esp_partition_erase_range(otadata, 0, otadata->size);
  if (err != ESP_OK) {
    ESP_LOGE("OTA", "otadata erase failed: %s", esp_err_to_name(err));
    return err;
  }
  err = esp_partition_write(otadata, 0, entry, sizeof(entry));
  if (err != ESP_OK) {
    ESP_LOGE("OTA", "otadata write failed: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGW("OTA", "Forced boot slot via otadata write (seq=%u -> %s)", (unsigned)seq, next->label);
  return ESP_OK;
}

void SettingsScreen::switch_ota_partition_() {
  auto running = esp_ota_get_running_partition();
  auto next = esp_ota_get_next_update_partition(running);
  ESP_LOGI("OTA", "Running: %s @ 0x%08lx", running ? running->label : "null",
           running ? (unsigned long)running->address : 0UL);
  ESP_LOGI("OTA", "Next:    %s @ 0x%08lx", next ? next->label : "null", next ? (unsigned long)next->address : 0UL);
  if (!next) {
    ESP_LOGE("OTA", "No next OTA partition found, aborting");
    return;
  }

  esp_err_t ret = esp_ota_set_boot_partition(next);
  if (ret == ESP_OK) {
    ESP_LOGI("OTA", "Switching to %s, restarting", next->label);
    esp_restart();
    return;
  }

  ESP_LOGW("OTA", "esp_ota_set_boot_partition refused (%s); falling back to direct otadata write",
           esp_err_to_name(ret));
  if (force_switch_via_otadata_(next) == ESP_OK) {
    ESP_LOGI("OTA", "Restarting into %s (unverified)", next->label);
    esp_restart();
  } else {
    ESP_LOGE("OTA", "Forced switch failed; staying on %s", running ? running->label : "running");
  }
}
#endif

void SettingsScreen::clear_cache_() {
  if (!data_dir_)
    return;
#ifdef ESP_PLATFORM
  char cache_dir[768];
  std::snprintf(cache_dir, sizeof(cache_dir), "%s/cache", data_dir_);
  DIR* d = opendir(cache_dir);
  if (!d) {
    mkdir(cache_dir, 0775);
    return;
  }
  struct dirent* ent;
  char subdir_path[768];
  while ((ent = readdir(d)) != nullptr) {
    if (ent->d_name[0] == '.')
      continue;
    std::snprintf(subdir_path, sizeof(subdir_path), "%s/%s", cache_dir, ent->d_name);
    // Remove all files inside the per-book subdir.
    DIR* sd = opendir(subdir_path);
    if (sd) {
      struct dirent* sf;
      char file_path[768];
      while ((sf = readdir(sd)) != nullptr) {
        if (sf->d_name[0] == '.')
          continue;
        std::snprintf(file_path, sizeof(file_path), "%s/%s", subdir_path, sf->d_name);
        std::remove(file_path);
      }
      closedir(sd);
    }
    rmdir(subdir_path);
  }
  closedir(d);
  rmdir(cache_dir);
  mkdir(cache_dir, 0775);
#else
  namespace fs = std::filesystem;
  try {
    std::string cache_path = std::string(data_dir_) + "/cache";
    fs::remove_all(cache_path);
    fs::create_directories(cache_path);
  } catch (...) {}
#endif
}

bool SettingsScreen::is_item_focusable(int index) const {
  if (!ListMenuScreen::is_item_focusable(index)) return false;
  const bool is_minimal = (theme() == MenuTheme::Minimal);
  const bool is_stele   = (theme() == MenuTheme::Stele);
  const bool is_codex   = (theme() == MenuTheme::Codex);
  const bool is_lyra    = (theme() == MenuTheme::Lyra);
  // List Align: only Minimal (and Lyra's child screens use Minimal fallback)
  if (index == idx_list_align_ && !is_minimal && !is_lyra) return false;
  // Nav Arrows: only Minimal has the bottom arrow bar; Lyra has its own tooltip
  if (index == idx_nav_arrows_ && !is_minimal) return false;
  // Battery Display: Stele/Codex/Lyra hardcode battery in their own headers
  if (index == idx_battery_display_ && (is_stele || is_codex || is_lyra)) return false;
  // Converted Mark: Stele always shows dots regardless of this toggle
  if (index == idx_conv_indicator_ && is_stele) return false;
  return true;
}

void SettingsScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  if (!ui_font_.valid() || !subtitle_font_.valid()) return;

  static constexpr int kLM = 14;
  static constexpr int kRM = 14;
  static constexpr int kRowH = 28;
  int y = 16;

  // ── Header: "Settings" left, version right ──────────────────────────────
  if (header_font_.valid()) {
    buf.draw_text_proportional(kLM, y + header_font_.baseline(), "Settings", header_font_, false);
    if (!subtitle_.empty()) {
      const BitmapFont& vfont = section_font_.valid() ? section_font_ : subtitle_font_;
      const int vw = vfont.word_width(subtitle_.c_str(), subtitle_.size(), FontStyle::Regular);
      const int vy = y + header_font_.y_advance() - vfont.y_advance() + vfont.baseline();
      buf.draw_text_proportional(W - kRM - vw, vy, subtitle_.c_str(), subtitle_.size(), vfont, false);
    }
    y += header_font_.y_advance();
  } else {
    buf.draw_text_proportional(kLM, y + ui_font_.baseline(), "Settings", ui_font_, false);
    y += ui_font_.y_advance();
  }
  y += 6;
  buf.fill_rect(0, y, W, 1, false);
  y += 1;

  // ── Item list ────────────────────────────────────────────────────────────
  const int n = count();
  const int list_top = y;

  // Only scroll if content actually overflows the available height
  int total_content_h = 0;
  for (int i = 0; i < n; ++i) {
    if (is_separator(i)) {
      total_content_h += 8;
      if (!get_item_label(i).empty() && section_font_.valid())
        total_content_h += section_font_.y_advance();
      total_content_h += 4;
    } else {
      total_content_h += kRowH;
    }
  }
  const int so = (total_content_h <= H - list_top) ? 0 : scroll_offset();

  for (int i = so; i < n && y < H; ++i) {
    if (is_separator(i)) {
      const std::string_view hdr = get_item_label(i);
      y += 8;
      if (y >= H) break;
      if (!hdr.empty()) {
        buf.draw_text_proportional(kLM, y + section_font_.baseline(),
                                   hdr.data(), hdr.size(), section_font_, false);
        y += section_font_.y_advance();
      }
      y += 4;
      continue;
    }

    const std::string_view label = get_item_label(i);
    std::string_view disp_label = label;
    std::string_view disp_value;
    const auto pos = label.find(": ");
    if (pos != std::string_view::npos) {
      disp_label = label.substr(0, pos);
      disp_value = label.substr(pos + 2);
    }

    const bool sel = (i == selected());
    if (sel)
      buf.fill_rect(0, y, W, kRowH, false);

    const int text_y = y + (kRowH - ui_font_.y_advance()) / 2 + ui_font_.baseline();
    buf.draw_text_proportional(kLM, text_y, disp_label.data(), disp_label.size(), ui_font_, sel);

    if (!disp_value.empty()) {
      const int vw = ui_font_.word_width(disp_value.data(), disp_value.size(), FontStyle::Regular);
      const int val_y = y + (kRowH - ui_font_.y_advance()) / 2 + ui_font_.baseline();
      buf.draw_text_proportional(W - kRM - vw, val_y, disp_value.data(), disp_value.size(), ui_font_, sel);
    }

    y += kRowH;
  }
}

}  // namespace microreader
