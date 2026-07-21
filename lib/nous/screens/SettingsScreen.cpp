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

static constexpr const char* kTabNames[4] = {"Look", "Reader", "Control", "System"};

// ---------------------------------------------------------------------------
// Label helpers
// ---------------------------------------------------------------------------

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
  if (path.empty())
    return "Sleep Image: Auto Rotate";
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

static std::string get_sleep_text_label(bool shown) {
  return std::string("Sleep Text: ") + (shown ? "Show" : "Hide");
}

static std::string get_reader_images_label(bool enabled) {
  return std::string("Book Images: ") + (enabled ? "On" : "Off");
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
  if (theme == 4) return "Theme: Lyra Like";
  if (theme == 5) return "Theme: Lyra Extended Like";
  return "Theme: Minimal";
}

static std::string get_sleep_timeout_label(uint8_t min) {
  if (min == 0) return "Auto Sleep: Off";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "Auto Sleep: %d min", static_cast<int>(min));
  return buf;
}

static std::string get_show_whats_new_label(bool on) {
  return std::string("Show on Update: ") + (on ? "On" : "Off");
}

static std::string get_font_label(const std::string& font_path) {
  std::string label = "Font: ";
  if (font_path == "Literata") {
    label += font_path;
  } else if (font_path.empty()) {
    label += "Literata";
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

// ---------------------------------------------------------------------------
// ensure_visible_ override — clamps scroll to never go below tab_start_
// ---------------------------------------------------------------------------

void SettingsScreen::ensure_visible_() {
  if (scroll_offset() < tab_start_[active_tab_])
    set_scroll_offset_(tab_start_[active_tab_]);
  ListMenuScreen::ensure_visible_();
  if (scroll_offset() < tab_start_[active_tab_])
    set_scroll_offset_(tab_start_[active_tab_]);
  // kPad in the base class can over-scroll short tabs. If the selected item is
  // already reachable from tab_start_, reset to tab_start_.
  const int H = current_height_();
  if (H > 0 && scroll_offset() > tab_start_[active_tab_]) {
    if (selected() - tab_start_[active_tab_] < get_visible_count_(H, tab_start_[active_tab_]))
      set_scroll_offset_(tab_start_[active_tab_]);
  }
}

// ---------------------------------------------------------------------------
// Tab bar drawing
// ---------------------------------------------------------------------------

int SettingsScreen::tab_bar_height_() const {
  return ui_font_.valid() ? ui_font_.y_advance() + 10 : 24;
}

void SettingsScreen::draw_tab_bar_(DrawBuffer& buf, int y, int W) const {
  const int tab_h = tab_bar_height_();
  const int tab_w = W / kTabCount;

  for (int i = 0; i < kTabCount; ++i) {
    const int tx = i * tab_w;
    const int tw = (i == kTabCount - 1) ? W - tx : tab_w;
    const bool is_active = (i == active_tab_);
    const bool inverted = is_active && (focus_state_ == FocusState::TabBar);

    if (inverted)
      buf.fill_rect(tx, y, tw, tab_h, false);

    if (!ui_font_.valid()) continue;
    const char* name = kTabNames[i];
    const int nw = static_cast<int>(ui_font_.word_width(name, strlen(name), FontStyle::Regular));
    const int cx = tx + tw / 2 - nw / 2;
    const int ty = y + (tab_h - ui_font_.y_advance()) / 2 + static_cast<int>(ui_font_.baseline());
    buf.draw_text_proportional(cx, ty, name, strlen(name), ui_font_, inverted);

    // Underline active tab when in list mode
    if (is_active && focus_state_ == FocusState::List)
      buf.fill_rect(tx, y + tab_h - 2, tw, 2, false);

    // Vertical divider between tabs
    if (i < kTabCount - 1)
      buf.fill_rect(tx + tw - 1, y + 4, 1, tab_h - 8, false);
  }

  // Bottom border
  buf.fill_rect(0, y + tab_h - 1, W, 1, false);
}

// ---------------------------------------------------------------------------
// on_start
// ---------------------------------------------------------------------------

void SettingsScreen::on_start() {
  title_ = "Settings";
  subtitle_ = MICROREADER_VERSION;
  focus_state_ = FocusState::TabBar;

  // Reset all idx
  idx_clear_cache_ = idx_rebuild_index_ = idx_list_format_ = idx_sort_order_ = -1;
  idx_switch_ota_ = idx_invalidate_font_ = idx_spiffs_ = -1;
  idx_invert_menu_ = idx_invert_bottom_paging_ = idx_invert_side_ = -1;
  idx_rotate_display_ = idx_reader_rotate_display_ = idx_menu_font_ = -1;
  idx_font_ = idx_sleep_image_ = idx_sleep_text_ = idx_reader_images_ = -1;
  idx_battery_display_ = idx_sleep_timeout_ = idx_convert_all_ = idx_theme_ = -1;
  idx_whats_new_ = idx_show_whats_new_ = -1;
#ifdef MICROREADER_ENABLE_DEMOS
  idx_bouncing_ball_ = idx_grayscale_demo_ = -1;
#endif

  // Build font list
  sd_fonts_.clear();
  sd_fonts_.push_back("Literata");
  font_sel_idx_ = 0;
#ifdef ESP_PLATFORM
  DIR* d = opendir("/sdcard/fonts");
  if (d) {
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
      if (ent->d_name[0] == '.') continue;
      const char* ext = std::strrchr(ent->d_name, '.');
      if (ext && strcmp(ext, ".mfb") == 0)
        sd_fonts_.push_back(std::string("/sdcard/fonts/") + ent->d_name);
    }
    closedir(d);
  }
#else
  namespace fs = std::filesystem;
  try {
    for (const auto& entry : fs::directory_iterator("sd/fonts")) {
      if (entry.path().extension() == ".mfb")
        sd_fonts_.push_back(entry.path().string());
    }
  } catch (...) {}
#endif
  if (app_) {
    const std::string& current = app_->custom_font_path();
    const std::string& match = (current.empty() || current == "Vollkorn" || current == "Alegreya") ? sd_fonts_[0] : current;
    for (size_t i = 0; i < sd_fonts_.size(); ++i) {
      if (sd_fonts_[i] == match) { font_sel_idx_ = static_cast<int>(i); break; }
    }
  }

  // An empty path enables the existing round-robin sleep-image mode.
  // The embedded "nous" image and custom SD images remain available to pin.
  sleep_images_.clear();
  sleep_images_.push_back("");  // Auto Rotate
  sleep_images_.push_back("embedded:0");  // nous — always present
  sleep_image_sel_idx_ = 0;
#ifdef ESP_PLATFORM
  {
    DIR* sd = opendir("/sdcard/.sleep");
    if (sd) {
      struct dirent* ent;
      while ((ent = readdir(sd)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
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
      if (sleep_images_[i] == current) { sleep_image_sel_idx_ = static_cast<int>(i); break; }
    }
  }

  // ── Tab 0: Appearance ───────────────────────────────────────────────────
  tab_start_[0] = count();

  idx_theme_ = count();
  add_item(get_theme_label(app_ ? app_->menu_theme() : 0));

  const uint8_t cur_theme = app_ ? app_->menu_theme() : 0;
  const bool is_lyra_theme = (cur_theme == static_cast<uint8_t>(MenuTheme::Lyra) ||
                               cur_theme == static_cast<uint8_t>(MenuTheme::LyraExt));
  if (!is_lyra_theme) {
    idx_rotate_display_ = count();
    add_item(get_rotate_menu_label(app_ ? app_->rotate_display() : 0));
  }

  idx_menu_font_ = count();
  add_item(get_menu_font_label(app_ ? app_->menu_font_size() : 0));

  idx_sleep_image_ = count();
  add_item(get_sleep_image_label(sleep_images_[sleep_image_sel_idx_]));

  idx_sleep_text_ = count();
  add_item(get_sleep_text_label(app_ ? app_->show_sleep_text() : true));

  idx_battery_display_ = count();
  add_item(get_battery_display_label(app_ ? app_->battery_display() : 0));

  tab_end_[0] = count() - 1;

  // ── Tab 1: Reader ────────────────────────────────────────────────────────
  tab_start_[1] = count();

  idx_reader_rotate_display_ = count();
  add_item(get_rotate_reader_label(app_ ? app_->rotate_reader() : 0));

  idx_font_ = count();
  add_item(get_font_label(sd_fonts_[font_sel_idx_]));

  idx_reader_images_ = count();
  add_item(get_reader_images_label(app_ ? app_->show_reader_images() : true));

  idx_list_format_ = count();
  if (app_) {
    add_item(get_list_format_label(app_->main_menu() ? app_->main_menu()->list_format() : BookListFormat::TitleOnly));
  } else {
    add_item("Book List: Title");
  }

  idx_sort_order_ = count();
  add_item(get_sort_order_label(app_ ? app_->sort_order() : BookSortOrder::Alphabetical));

  idx_sleep_timeout_ = count();
  add_item(get_sleep_timeout_label(app_ ? app_->sleep_timeout_min() : 10));

  tab_end_[1] = count() - 1;

  // ── Tab 2: Control ───────────────────────────────────────────────────────
  tab_start_[2] = count();

  idx_invert_side_ = count();
  add_item(get_side_paging_label(app_ ? app_->invert_side_buttons() : false));

  idx_invert_bottom_paging_ = count();
  add_item(get_bottom_paging_label(app_ ? app_->invert_bottom_paging() : true));

  idx_invert_menu_ = count();
  add_item(get_menu_nav_label(app_ ? app_->invert_menu_buttons() : false));

  tab_end_[2] = count() - 1;

  // ── Tab 3: System ────────────────────────────────────────────────────────
  tab_start_[3] = count();

  if (data_dir_) {
    idx_clear_cache_ = count();
    add_item("Clear Cache");

    idx_rebuild_index_ = count();
    add_item("Rebuild Book Index");
  }

  idx_convert_all_ = count();
  add_item("Convert All Books");

  idx_whats_new_ = count();
  add_item("What's New");

  idx_show_whats_new_ = count();
  add_item(get_show_whats_new_label(app_ ? app_->show_whats_new_on_update() : true));

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

#ifdef MICROREADER_ENABLE_DEMOS
  idx_bouncing_ball_ = count();
  add_item("Bouncing Ball");

  idx_grayscale_demo_ = count();
  add_item("Grayscale Demo");
#endif

  tab_end_[3] = count() - 1;

  // Pin scroll and selection to the active tab's start so returning from a
  // sub-screen (which resets scroll_offset_ to 0 via clear_items) never
  // shows items from other tabs.
  set_selected(tab_start_[active_tab_]);
  set_scroll_offset_(tab_start_[active_tab_]);
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void SettingsScreen::on_back() {
  if (focus_state_ == FocusState::List) {
    focus_state_ = FocusState::TabBar;
    request_redraw();
    // Do NOT call pop_screen — just return to tab bar
  } else {
    ListMenuScreen::on_back();
  }
}

int SettingsScreen::get_visible_count_(int H, int scroll_off) const {
  // Before this tab's range starts, nothing is visible — forces ensure_visible_()
  // to advance scroll_offset_ to tab_start_ before counting begins.
  if (scroll_off < tab_start_[active_tab_]) return 0;
  const int list_top = 16
      + (header_font_.valid() ? header_font_.y_advance() : ui_font_.y_advance())
      + 7 + tab_bar_height_();
  const int available_h = H - list_top;
  int h = 0, cnt = 0;
  const int end = tab_end_[active_tab_];
  for (int i = scroll_off; i <= end; ++i) {
    if (h + kRowH > available_h) break;
    h += kRowH;
    ++cnt;
  }
  return cnt;
}

bool SettingsScreen::is_item_focusable(int index) const {
  if (!ListMenuScreen::is_item_focusable(index)) return false;
  // Only items within the active tab are reachable
  if (index < tab_start_[active_tab_] || index > tab_end_[active_tab_]) return false;
  return true;
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------

void SettingsScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;

  // Toast countdown
  if (toast_frames_ > 0) {
    --toast_frames_;
    if (toast_frames_ == 0 && toast_idx_ >= 0) {
      set_item_label(toast_idx_, toast_original_label_);
      toast_idx_ = -1;
      toast_original_label_.clear();
      request_redraw();
    }
  }

  // Picker takes priority
  if (picker_open_) {
    const bool inv = app_ && app_->invert_menu_buttons();
    const Button btn_up   = inv ? Button::Button2 : Button::Button3;
    const Button btn_down = inv ? Button::Button3 : Button::Button2;
    const int n = static_cast<int>(picker_options_.size());
    bool redraw = false;
    Button btn;
    while (buttons.next_press(btn)) {
      if (btn == btn_up || btn == Button::Up) {
        picker_sel_ = (picker_sel_ - 1 + n) % n;
        redraw = true;
      } else if (btn == btn_down || btn == Button::Down) {
        picker_sel_ = (picker_sel_ + 1) % n;
        redraw = true;
      } else if (btn == Button::Button1) {
        apply_picker_(picker_sel_);
        return;
      } else if (btn == Button::Button0) {
        picker_open_ = false;
        redraw = true;
      }
    }
    if (redraw) {
      draw_all_(buf, runtime.battery_percentage());
      buf.refresh();
    }
    return;
  }

  // Tab bar focus
  if (focus_state_ == FocusState::TabBar) {
    const bool inv = app_ && app_->invert_menu_buttons();
    const Button btn_down_front = inv ? Button::Button3 : Button::Button2;
    bool redraw = false;
    Button btn;
    while (buttons.next_press(btn)) {
      if (btn == Button::Button1) {
        // Select: cycle to next tab
        active_tab_ = (active_tab_ + 1) % kTabCount;
        set_selected(tab_start_[active_tab_]);
        set_scroll_offset_(tab_start_[active_tab_]);
        redraw = true;
      } else if (btn == btn_down_front || btn == Button::Down) {
        // Down: enter list at first item of this tab
        focus_state_ = FocusState::List;
        set_selected(tab_start_[active_tab_]);
        set_scroll_offset_(tab_start_[active_tab_]);
        redraw = true;
      } else if (btn == Button::Button0) {
        // Back: exit to main menu
        if (app_) app_->pop_screen();
        return;
      }
    }
    if (redraw) {
      draw_all_(buf, runtime.battery_percentage());
      buf.refresh();
    }
    return;
  }

  // List focus — delegate to base class (it calls on_select / on_back)
  ListMenuScreen::update(buttons, buf, runtime);
}

// ---------------------------------------------------------------------------
// Picker
// ---------------------------------------------------------------------------

void SettingsScreen::open_picker_(const char* title, int target_idx, std::vector<std::string> opts, int cur_sel) {
  picker_title_   = title;
  picker_target_  = target_idx;
  picker_options_ = std::move(opts);
  picker_sel_     = cur_sel;
  picker_open_    = true;
  request_redraw();
}

void SettingsScreen::apply_picker_(int sel) {
  picker_open_ = false;
  if (!app_) return;

  if (picker_target_ == idx_theme_) {
    static constexpr uint8_t kThemeOrder[] = {1, 0, 2, 3, 4, 5};
    const uint8_t old_v = app_->menu_theme();
    const uint8_t v = kThemeOrder[sel >= 0 && sel < 6 ? sel : 0];
    app_->set_menu_theme(v);
    set_item_label(idx_theme_, get_theme_label(v));
    if (v == static_cast<uint8_t>(ListMenuScreen::MenuTheme::Lyra))
      app_->replace_screen(ScreenId::Lyra);
    else if (v == static_cast<uint8_t>(ListMenuScreen::MenuTheme::LyraExt))
      app_->replace_screen(ScreenId::LyraExt);
    else if (old_v == static_cast<uint8_t>(ListMenuScreen::MenuTheme::Lyra) ||
             old_v == static_cast<uint8_t>(ListMenuScreen::MenuTheme::LyraExt))
      app_->replace_screen(ScreenId::MainMenu);
    else
      app_->pop_screen();
    return;
  }
  if (picker_target_ == idx_menu_font_) {
    app_->set_menu_font_size(sel);
    const int saved_tab = active_tab_;
    restart();  // rebuilds items + fonts; resets active_tab_ and focus_state_
    active_tab_ = saved_tab;
    focus_state_ = FocusState::List;
    set_selected(idx_menu_font_);
    ensure_visible_();
    return;
  }
  if (picker_target_ == idx_battery_display_) {
    auto v = static_cast<uint8_t>(sel);
    app_->set_battery_display(v);
    set_item_label(idx_battery_display_, get_battery_display_label(v));
  } else if (picker_target_ == idx_sleep_timeout_) {
    static constexpr uint8_t kTimeouts[] = {0, 1, 3, 5, 10, 20, 30};
    uint8_t v = (sel >= 0 && sel < (int)sizeof(kTimeouts)) ? kTimeouts[sel] : 0;
    app_->set_sleep_timeout_min(v);
    set_item_label(idx_sleep_timeout_, get_sleep_timeout_label(v));
  } else if (picker_target_ == idx_list_format_) {
    static const BookListFormat kFormats[] = {
      BookListFormat::TitleOnly, BookListFormat::Filename, BookListFormat::TitleAndAuthor
    };
    auto fmt = (sel >= 0 && sel < 3) ? kFormats[sel] : BookListFormat::TitleOnly;
    if (app_->main_menu()) {
      app_->main_menu()->set_list_format(fmt);
      set_item_label(idx_list_format_, get_list_format_label(fmt));
    }
    app_->save_settings_();
  } else if (picker_target_ == idx_rotate_display_) {
    auto v = static_cast<uint8_t>(sel);
    app_->set_rotate_display(v);
    set_item_label(idx_rotate_display_, get_rotate_menu_label(v));
    if (buf_) buf_->set_rotation(rotation_from_setting(v));
  } else if (picker_target_ == idx_reader_rotate_display_) {
    auto v = static_cast<uint8_t>(sel);
    app_->set_rotate_reader(v);
    set_item_label(idx_reader_rotate_display_, get_rotate_reader_label(v));
  } else if (picker_target_ == idx_font_) {
    if (sel >= 0 && sel < (int)sd_fonts_.size()) {
      font_sel_idx_ = sel;
      app_->set_custom_font_path(sd_fonts_[sel]);
      set_item_label(idx_font_, get_font_label(sd_fonts_[sel]));
    }
  } else if (picker_target_ == idx_sleep_image_) {
    if (sel >= 0 && sel < (int)sleep_images_.size()) {
      sleep_image_sel_idx_ = sel;
      app_->set_sleep_image_path(sleep_images_[sel]);
      set_item_label(idx_sleep_image_, get_sleep_image_label(sleep_images_[sel]));
    }
  }
  request_redraw();
}

// ---------------------------------------------------------------------------
// on_select
// ---------------------------------------------------------------------------

void SettingsScreen::on_select(int index) {
  if (index == idx_theme_) {
    // Picker order: Minimal(1), Chronicle(0), Stele(2), Codex(3), Lyra(4), LyraExt(5)
    static constexpr uint8_t kThemeOrder[] = {1, 0, 2, 3, 4, 5};
    int cur_sel = 0;
    if (app_) {
      const uint8_t cur = app_->menu_theme();
      for (int i = 0; i < 6; ++i)
        if (kThemeOrder[i] == cur) { cur_sel = i; break; }
    }
    open_picker_("Select Theme", idx_theme_,
      {"Minimal", "Chronicle", "Stele", "Codex", "Lyra Like", "Lyra Extended Like"},
      cur_sel);
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
  if (index == idx_sleep_text_) {
    if (app_) {
      bool v = !app_->show_sleep_text();
      app_->set_show_sleep_text(v);
      set_item_label(idx_sleep_text_, get_sleep_text_label(v));
    }
    return;
  }
  if (index == idx_battery_display_) {
    open_picker_("Battery Display", idx_battery_display_,
      {"Icon", "Number", "Icon & Number"},
      app_ ? static_cast<int>(app_->battery_display()) : 0);
    return;
  }
  if (index == idx_sleep_timeout_) {
    static constexpr uint8_t kTimeouts[] = {0, 1, 3, 5, 10, 20, 30};
    int cur_idx = 0;
    if (app_) {
      uint8_t cur = app_->sleep_timeout_min();
      for (int i = 0; i < (int)sizeof(kTimeouts); ++i)
        if (kTimeouts[i] == cur) { cur_idx = i; break; }
    }
    open_picker_("Auto Sleep", idx_sleep_timeout_,
      {"Off", "1 min", "3 min", "5 min", "10 min", "20 min", "30 min"},
      cur_idx);
    return;
  }
  if (index == idx_convert_all_) {
    if (app_) app_->push_screen(ScreenId::ConvertAll);
    return;
  }
  if (index == idx_whats_new_) {
    if (app_) app_->push_screen(ScreenId::WhatsNew);
    return;
  }
  if (index == idx_show_whats_new_) {
    if (app_) {
      bool v = !app_->show_whats_new_on_update();
      app_->set_show_whats_new_on_update(v);
      set_item_label(idx_show_whats_new_, get_show_whats_new_label(v));
    }
    return;
  }
  if (index == idx_sort_order_) {
    if (app_) {
      BookSortOrder order = (app_->sort_order() == BookSortOrder::Alphabetical)
          ? BookSortOrder::LastOpened : BookSortOrder::Alphabetical;
      app_->set_sort_order(order);
      set_item_label(idx_sort_order_, get_sort_order_label(order));
    }
    return;
  }
  if (index == idx_list_format_) {
    int cur = 0;
    if (app_ && app_->main_menu()) {
      auto fmt = app_->main_menu()->list_format();
      if (fmt == BookListFormat::Filename) cur = 1;
      else if (fmt == BookListFormat::TitleAndAuthor) cur = 2;
    }
    open_picker_("Book List Format", idx_list_format_,
      {"Title Only", "Filename", "Title & Author"}, cur);
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
    open_picker_("Menu Rotation", idx_rotate_display_,
      {"Normal", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0"},
      app_ ? static_cast<int>(app_->rotate_display()) : 0);
    return;
  }
  if (index == idx_reader_rotate_display_) {
    open_picker_("Reader Rotation", idx_reader_rotate_display_,
      {"Normal", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0"},
      app_ ? static_cast<int>(app_->rotate_reader()) : 0);
    return;
  }
  if (index == idx_menu_font_) {
    open_picker_("Menu Size", idx_menu_font_,
      {"Small", "Medium", "Large", "X-Large"},
      app_ ? app_->menu_font_size() : 0);
    return;
  }
  if (index == idx_font_) {
    std::vector<std::string> font_names;
    font_names.reserve(sd_fonts_.size());
    for (const auto& fp : sd_fonts_) font_names.push_back(get_font_label(fp).substr(6));
    open_picker_("Font", idx_font_, std::move(font_names), font_sel_idx_);
    return;
  }
  if (index == idx_sleep_image_) {
    std::vector<std::string> img_names;
    img_names.reserve(sleep_images_.size());
    for (const auto& ip : sleep_images_) img_names.push_back(get_sleep_image_label(ip).substr(13));
    open_picker_("Sleep Image", idx_sleep_image_, std::move(img_names), sleep_image_sel_idx_);
    return;
  }
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
      app_->pop_screen();
    }
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
        if (buf_) buf_->show_loading("Writing...", 50);

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
          if (in_left > in_sz) flags |= TINFL_FLAG_HAS_MORE_INPUT;
          status = tinfl_decompress(decomp, in_ptr, &in_sz, dict, dict + dict_ofs, &out_sz, flags);
          in_ptr += in_sz;
          in_left -= in_sz;
          size_t produced = out_sz;
          size_t write_ofs = 0;
          while (write_ofs < produced) {
            size_t chunk = produced - write_ofs;
            if (chunk > kWriteSize) chunk = kWriteSize;
            memcpy(wbuf, dict + dict_ofs + write_ofs, chunk);
            esp_partition_write(part, flash_offset, wbuf, chunk);
            flash_offset += chunk;
            write_ofs += chunk;
          }
          dict_ofs = (dict_ofs + produced) & (kDictSize - 1);
          if (status <= TINFL_STATUS_DONE) break;
        }
        free(work);
        buf_->show_loading("Done!", 100);
      }
    }
    esp_restart();
  }
#endif
}

// ---------------------------------------------------------------------------
// Subtitle (value column for list items)
// ---------------------------------------------------------------------------

std::string_view SettingsScreen::get_item_subtitle(int index) const {
  std::string_view label = ListMenuScreen::get_item_label(index);
  const auto pos = label.find(": ");
  if (pos == std::string_view::npos) return {};
  subtitle_buf_ = label.substr(pos + 2);
  return subtitle_buf_;
}

// ---------------------------------------------------------------------------
// draw_all_
// ---------------------------------------------------------------------------

void SettingsScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  if (!ui_font_.valid()) return;

  static constexpr int kLM = 14;
  static constexpr int kRM = 14;
  int y = 16;

  // ── Header ───────────────────────────────────────────────────────────────
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

  // ── Tab bar ──────────────────────────────────────────────────────────────
  draw_tab_bar_(buf, y, W);
  y += tab_bar_height_();

  // ── Item list (active tab only) ───────────────────────────────────────────
  const int range_end = tab_end_[active_tab_];
  const int so = scroll_offset();

  for (int i = so; i <= range_end && y < H; ++i) {
    const std::string_view label = get_item_label(i);
    std::string_view disp_label = label;
    std::string_view disp_value;
    const auto pos = label.find(": ");
    if (pos != std::string_view::npos) {
      disp_label = label.substr(0, pos);
      disp_value = label.substr(pos + 2);
    }

    const bool sel = (focus_state_ == FocusState::List) && (i == selected());
    if (sel)
      buf.fill_rect(0, y, W, kRowH, false);

    const int text_y = y + (kRowH - ui_font_.y_advance()) / 2 + ui_font_.baseline();
    buf.draw_text_proportional(kLM, text_y, disp_label.data(), disp_label.size(), ui_font_, sel);

    if (!disp_value.empty()) {
      const int vw = ui_font_.word_width(disp_value.data(), disp_value.size(), FontStyle::Regular);
      buf.draw_text_proportional(W - kRM - vw, text_y, disp_value.data(), disp_value.size(), ui_font_, sel);
    }

    y += kRowH;
  }

  // ── Picker overlay ───────────────────────────────────────────────────────
  if (picker_open_ && ui_font_.valid())
    draw_picker_(buf);
}

// ---------------------------------------------------------------------------
// Picker drawing
// ---------------------------------------------------------------------------

void SettingsScreen::draw_picker_(DrawBuffer& buf) const {
  if (!ui_font_.valid()) return;
  const int W = buf.width();
  const int H = buf.height();
  const int n = static_cast<int>(picker_options_.size());

  static constexpr int kPickerPadH = 20;
  static constexpr int kRowPad     = 8;
  static constexpr int kTitlePad   = 8;
  const int row_h   = kRowPad + ui_font_.y_advance() + kRowPad;
  const int title_h = kTitlePad + ui_font_.y_advance() + kTitlePad;
  const int popup_h = 1 + title_h + 1 + n * row_h + 1;
  const int popup_x = kPickerPadH;
  const int popup_w = W - 2 * kPickerPadH;
  const int popup_y = std::max(0, (H - popup_h) / 2);
  const int text_x  = popup_x + 1 + 10;

  buf.fill_rect(popup_x, popup_y, popup_w, popup_h, true);
  buf.fill_rect(popup_x, popup_y, popup_w, 1, false);
  buf.fill_rect(popup_x, popup_y + popup_h - 1, popup_w, 1, false);
  buf.fill_rect(popup_x, popup_y, 1, popup_h, false);
  buf.fill_rect(popup_x + popup_w - 1, popup_y, 1, popup_h, false);

  int py = popup_y + 1;
  buf.draw_text_proportional(text_x, py + kTitlePad + ui_font_.baseline(),
                             picker_title_.c_str(), picker_title_.size(), ui_font_, false);
  py += title_h;
  buf.fill_rect(popup_x + 1, py, popup_w - 2, 1, false);
  py += 1;

  for (int i = 0; i < n; ++i) {
    const bool sel = (i == picker_sel_);
    if (sel)
      buf.fill_rect(popup_x + 1, py, popup_w - 2, row_h, false);
    buf.draw_text_proportional(text_x, py + kRowPad + ui_font_.baseline(),
                               picker_options_[i].c_str(), picker_options_[i].size(), ui_font_, sel);
    py += row_h;
  }
  buf.fill_rect(popup_x, popup_y + popup_h - 1, popup_w, 1, false);
}

// ---------------------------------------------------------------------------
// Cache clearing
// ---------------------------------------------------------------------------

void SettingsScreen::clear_cache_() {
  if (!data_dir_) return;
#ifdef ESP_PLATFORM
  char cache_dir[768];
  std::snprintf(cache_dir, sizeof(cache_dir), "%s/cache", data_dir_);
  DIR* d = opendir(cache_dir);
  if (!d) { mkdir(cache_dir, 0775); return; }
  struct dirent* ent;
  char subdir_path[768];
  while ((ent = readdir(d)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    std::snprintf(subdir_path, sizeof(subdir_path), "%s/%s", cache_dir, ent->d_name);
    DIR* sd = opendir(subdir_path);
    if (sd) {
      struct dirent* sf;
      char file_path[768];
      while ((sf = readdir(sd)) != nullptr) {
        if (sf->d_name[0] == '.') continue;
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

// ---------------------------------------------------------------------------
// OTA partition switch (ESP only)
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
static esp_err_t force_switch_via_otadata_(const esp_partition_t* next) {
  if (!next) return ESP_ERR_INVALID_ARG;
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata) { ESP_LOGE("OTA", "otadata partition not found"); return ESP_ERR_NOT_FOUND; }

  uint32_t seq = (next->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) ? 1u : 2u;
  uint8_t entry[32];
  std::memset(entry, 0xFF, sizeof(entry));
  std::memcpy(entry, &seq, 4);
  uint32_t crc = esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&seq), 4);
  std::memcpy(entry + 28, &crc, 4);

  esp_err_t err = esp_partition_erase_range(otadata, 0, otadata->size);
  if (err != ESP_OK) { ESP_LOGE("OTA", "otadata erase failed: %s", esp_err_to_name(err)); return err; }
  err = esp_partition_write(otadata, 0, entry, sizeof(entry));
  if (err != ESP_OK) { ESP_LOGE("OTA", "otadata write failed: %s", esp_err_to_name(err)); return err; }
  ESP_LOGW("OTA", "Forced boot slot via otadata write (seq=%u -> %s)", (unsigned)seq, next->label);
  return ESP_OK;
}

void SettingsScreen::switch_ota_partition_() {
  auto running = esp_ota_get_running_partition();
  auto next = esp_ota_get_next_update_partition(running);
  if (!next) { ESP_LOGE("OTA", "No next OTA partition found, aborting"); return; }

  esp_err_t ret = esp_ota_set_boot_partition(next);
  if (ret == ESP_OK) { esp_restart(); return; }

  ESP_LOGW("OTA", "esp_ota_set_boot_partition refused (%s); falling back to direct otadata write",
           esp_err_to_name(ret));
  if (force_switch_via_otadata_(next) == ESP_OK)
    esp_restart();
  else
    ESP_LOGE("OTA", "Forced switch failed; staying on %s", running ? running->label : "running");
}
#endif

}  // namespace microreader
