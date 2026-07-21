#pragma once

#include <string>
#include <vector>

#include "../Input.h"
#include "../display/DrawBuffer.h"
#include "ListMenuScreen.h"

namespace microreader {

class SettingsScreen final : public ListMenuScreen {
 public:
  SettingsScreen() = default;

  void set_data_dir(const char* dir) {
    data_dir_ = dir;
  }

  const char* name() const override {
    return "Settings";
  }

  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

  std::string_view get_item_subtitle(int index) const override;
  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;
  bool is_item_focusable(int index) const override;
  int get_visible_count_(int H, int scroll_off) const override;

  static constexpr int kRowH = 28;

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override;

 private:
  const char* data_dir_ = nullptr;

  // Tab navigation state
  enum class FocusState : uint8_t { TabBar, List };
  FocusState focus_state_ = FocusState::TabBar;
  int active_tab_ = 0;
  static constexpr int kTabCount = 4;
  int tab_start_[kTabCount] = {};
  int tab_end_[kTabCount] = {};

  // Item indices (assigned during on_start).
#ifdef MICROREADER_ENABLE_DEMOS
  int idx_bouncing_ball_ = -1;
  int idx_grayscale_demo_ = -1;
#endif

  int idx_clear_cache_ = -1;
  int idx_rebuild_index_ = -1;
  int idx_list_format_ = -1;
  int idx_sort_order_ = -1;
  int idx_switch_ota_ = -1;
  int idx_invalidate_font_ = -1;
  int idx_spiffs_ = -1;
  int idx_invert_menu_ = -1;
  int idx_invert_bottom_paging_ = -1;
  int idx_invert_side_ = -1;
  int idx_rotate_display_ = -1;
  int idx_reader_rotate_display_ = -1;
  int idx_menu_font_ = -1;
  int idx_font_ = -1;
  int idx_sleep_image_ = -1;
  int idx_sleep_text_ = -1;
  int idx_reader_images_ = -1;
  int idx_battery_display_ = -1;
  int idx_sleep_timeout_ = -1;
  int idx_convert_all_ = -1;
  int idx_theme_ = -1;

  DrawBuffer* buf_ = nullptr;
  mutable std::string subtitle_buf_;
  std::vector<std::string> sd_fonts_;
  int font_sel_idx_ = 0;
  std::vector<std::string> sleep_images_;
  int sleep_image_sel_idx_ = 0;
  int toast_idx_ = -1;
  std::string toast_original_label_;
  int toast_frames_ = 0;

  // Generic popup picker for any multi-choice setting.
  bool picker_open_   = false;
  int  picker_sel_    = 0;
  int  picker_target_ = -1;
  std::string picker_title_;
  std::vector<std::string> picker_options_;

  void ensure_visible_() override;
  int tab_bar_height_() const;
  void draw_tab_bar_(DrawBuffer& buf, int y, int W) const;

  void open_picker_(const char* title, int target_idx, std::vector<std::string> opts, int cur_sel);
  void apply_picker_(int sel);
  void draw_picker_(DrawBuffer& buf) const;
  void clear_cache_();
#ifdef ESP_PLATFORM
  void switch_ota_partition_();
#endif
};

}  // namespace microreader
