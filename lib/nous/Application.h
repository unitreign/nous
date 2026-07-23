#pragma once

#include <cstdint>
#include <functional>

#include "FontManager.h"
#include "Input.h"
#include "Runtime.h"
#include "ScreenManager.h"
#include "display/DrawBuffer.h"
#include "screens/ChapterSelectScreen.h"
#include "screens/ConvertAllScreen.h"
#include "screens/HiddenBooksMenu.h"
#include "screens/IScreen.h"
#include "screens/LinksScreen.h"
#include "screens/LyraExtScreen.h"
#include "screens/LyraScreen.h"
#include "screens/MainMenu.h"
#include "screens/RecentBooksScreen.h"
#include "screens/ReaderOptionsScreen.h"
#include "screens/ReaderScreen.h"
#include "screens/SettingsScreen.h"
#include "screens/StatsScreen.h"
#include "screens/WhatsNewScreen.h"
#include "screens/demo/BouncingBallDemo.h"
#include "screens/demo/GrayscaleDemo.h"

namespace microreader {

// All navigable screens in the application.
enum class ScreenId : uint8_t {
  None = 0,
  MainMenu,
  Reader,
  Settings,
  ReaderOptions,
  ChapterSelect,
  Links,
  ConvertAll,
  Stats,
  HiddenBooks,
  Lyra,
  LyraExt,
  RecentBooks,
  WhatsNew,
  BouncingBall,
  GrayscaleDemo,
};

// Maps the rotate_display / rotate_reader setting value (0-3) to the DrawBuffer Rotation enum.
// 0=Portrait(Deg90), 1=Landscape(Deg0), 2=Portrait-Flip(Deg270), 3=Landscape-Flip(Deg180)
inline Rotation rotation_from_setting(uint8_t v) {
  switch (v) {
    case 1:  return Rotation::Deg0;
    case 2:  return Rotation::Deg270;
    case 3:  return Rotation::Deg180;
    default: return Rotation::Deg90;
  }
}

inline const char* rotation_label(uint8_t v) {
  switch (v) {
    case 1:  return "Landscape";
    case 2:  return "Portrait (Flip)";
    case 3:  return "Landscape (Flip)";
    default: return "Portrait";
  }
}

class Application {
 public:
  Application() = default;

  void set_books_dir(const char* dir) {
    menu_.set_books_dir(dir);
  }

  void set_data_dir(const char* dir) {
    data_dir_ = dir;
    reader_.set_data_dir(dir ? dir : "");
    settings_.set_data_dir(dir);
  }

  // Path to data directory for settings/state persistence
  const char* data_dir_ = nullptr;

  // Path to the single unified settings file (cached after set_data_dir)
  std::string settings_path_;
  // Book path to auto-open on next start() (set by load_settings_)
  std::string pending_book_path_;
  // Screen to auto-open on next start() (set by load_settings_)
  std::string pending_screen_;

  // Save all persistent state to the settings file
  void save_settings_();
  // Load all persistent state from the settings file
  void load_settings_();
  // Common sleep sequence (save state, show sleep image, set running_=false)
  void do_sleep_(DrawBuffer& buf);

  // Font management. set_reader_font() also propagates to the reader screen.
  void set_reader_font(const BitmapFontSet* fonts) {
    reader_font_ = fonts;
    reader_.set_fonts(fonts);
  }
  void set_font_manager(FontManager* fm) {
    font_manager_ = fm;
  }
  FontManager* font_manager() const {
    return font_manager_;
  }

  // Optional callback for "Invalidate Font" in the Settings menu (ESP32 only).
  void set_invalidate_font_fn(std::function<void()> fn) {
    invalidate_font_fn_ = std::move(fn);
  }
  void invalidate_font() {
    if (invalidate_font_fn_)
      invalidate_font_fn_();
  }
  bool has_invalidate_font_fn() const {
    return static_cast<bool>(invalidate_font_fn_);
  }

  ReaderScreen* reader() {
    return &reader_;
  }
  // Returns true when the Reader is the top/active screen — used by the main
  // loop to decide whether scratch-needing index ops (Add/Rename) must be
  // deferred to avoid corrupting display buffers the Reader is rendering to.
  bool is_reader_active() const {
    return screen_mgr_.top() == &reader_;
  }
  // Returns the name() of the top/active screen for serial diagnostics.
  const char* top_screen_name() const {
    IScreen* top = screen_mgr_.top();
    return top ? top->name() : "none";
  }
  SettingsScreen* settings() {
    return &settings_;
  }
  ReaderOptionsScreen* reader_options() {
    return &reader_options_;
  }
  ChapterSelectScreen* chapter_select() {
    return &chapter_select_;
  }
  LinksScreen* links_screen() {
    return &links_screen_;
  }
  MainMenu* main_menu() {
    return &menu_;
  }
  LyraScreen* lyra_screen() {
    return &lyra_;
  }
  LyraExtScreen* lyra_ext_screen() {
    return &lyra_ext_;
  }
  RecentBooksScreen* recent_books_screen() {
    return &recent_books_;
  }
  ConvertAllScreen* convert_all_screen() {
    return &convert_all_;
  }
  StatsScreen* stats_screen() {
    return &stats_;
  }

  bool show_nav_arrows() const { return show_nav_arrows_; }
  void set_show_nav_arrows(bool v) { show_nav_arrows_ = v; save_settings_(); }

  bool show_sleep_text() const { return show_sleep_text_; }
  void set_show_sleep_text(bool v) { show_sleep_text_ = v; save_settings_(); }

  bool show_whats_new_on_update() const { return show_whats_new_on_update_; }
  void set_show_whats_new_on_update(bool v) { show_whats_new_on_update_ = v; save_settings_(); }

  bool show_reader_images() const { return show_reader_images_; }
  void set_show_reader_images(bool v);

  bool show_converted_indicator() const { return show_converted_indicator_; }
  void set_show_converted_indicator(bool v) { show_converted_indicator_ = v; save_settings_(); }

  uint8_t battery_display() const { return battery_display_; }
  void set_battery_display(uint8_t v) { battery_display_ = v <= 2 ? v : 0; save_settings_(); }

  uint8_t list_align() const { return list_align_; }
  void set_list_align(uint8_t v) { list_align_ = v <= 2 ? v : 0; save_settings_(); }

  uint8_t menu_theme() const { return menu_theme_; }
  void set_menu_theme(uint8_t v);

  void update_book_read_time(const std::string& path, uint64_t ms);

  uint8_t sleep_timeout_min() const { return sleep_timeout_min_; }
  void set_sleep_timeout_min(uint8_t v) { sleep_timeout_min_ = v; save_settings_(); }

  bool invert_menu_buttons() const {
    return invert_menu_buttons_;
  }
  void set_invert_menu_buttons(bool v) {
    invert_menu_buttons_ = v;
  }

  bool invert_bottom_paging() const {
    return invert_bottom_paging_;
  }
  void set_invert_bottom_paging(bool v) {
    invert_bottom_paging_ = v;
  }

  bool invert_side_buttons() const {
    return invert_side_buttons_;
  }
  void set_invert_side_buttons(bool v) {
    invert_side_buttons_ = v;
  }

  uint8_t rotate_display() const {
    return rotate_display_;
  }
  void set_rotate_display(uint8_t v) {
    rotate_display_ = v <= 3 ? v : 0;
    save_settings_();
  }

  uint8_t rotate_reader() const {
    return rotate_reader_;
  }
  void set_rotate_reader(uint8_t v) {
    rotate_reader_ = v <= 3 ? v : 0;
    save_settings_();
  }

  int menu_font_size() const {
    return menu_font_size_;
  }
  void set_menu_font_size(int v) {
    menu_font_size_ = v;
    ListMenuScreen::set_font_size(v);
    save_settings_();
  }

  const std::string& custom_font_path() const {
    return custom_font_path_;
  }
  void set_custom_font_path(const std::string& path) {
    custom_font_path_ = path;
    save_settings_();
  }

  // Empty string = auto-cycle through all images on each sleep.
  const std::string& sleep_image_path() const {
    return sleep_image_path_;
  }
  void set_sleep_image_path(const std::string& path) {
    sleep_image_path_ = path;
    save_settings_();
  }

  const std::string& installed_font_path() const {
    return installed_font_path_;
  }
  void set_installed_font_path(const std::string& path) {
    installed_font_path_ = path;
    save_settings_();
  }

  BookSortOrder sort_order() const {
    return menu_.sort_order();
  }
  void set_sort_order(BookSortOrder order) {
    menu_.set_sort_order(order);
    save_settings_();
  }

  // Called by MainMenu when the user opens a book: updates the open-order
  // counter in the index and persists both the index and settings.
  void record_book_opened(const std::string& path);

  // Extract cover.bin for the given EPUB if it doesn't exist or is stale.
  // No-op if data_dir is not set or the EPUB has no cover.
  // Blocking — can take ~1s. Pass scratch bufs when available to avoid heap pressure.
  void ensure_cover_bin(const std::string& epub_path,
                        uint8_t* scratch1 = nullptr, uint8_t* scratch2 = nullptr,
                        size_t scratch_size = 0);

  // Navigate to a screen: push on top of the current screen (current stays on stack).
  // Or replace the current screen (pop it first, then push the new one).
  // safe to call from within a screen's update(); the transition happens after update() returns.
  void push_screen(ScreenId id) {
    pending_push_ = id;
    pending_replace_ = ScreenId::None;
    pending_pop_count_ = 0;
  }
  void replace_screen(ScreenId id) {
    pending_push_ = ScreenId::None;
    pending_replace_ = id;
    pending_pop_count_ = 0;
  }
  void pop_screen(int count = 1) {
    pending_push_ = ScreenId::None;
    pending_replace_ = ScreenId::None;
    pending_pop_count_ = count;
  }

  bool has_pending_transition() const {
    return pending_push_ != ScreenId::None || pending_replace_ != ScreenId::None || pending_pop_count_ > 0;
  }

  void start(DrawBuffer& buf, IRuntime& runtime);

  // Reset the inactivity timer so the device won't sleep. Call each tick
  // whenever an external connection (e.g. USB serial) is active.
  void keep_awake() { inactivity_ms_ = 0; }
  // Auto-open a book by path (skips menu, for debugging).
  void auto_open_book(const char* epub_path, DrawBuffer& buf, IRuntime& runtime);
  void update(const ButtonState& buttons, uint32_t dt_ms, DrawBuffer& buf, IRuntime& runtime);
  bool running() const;
  uint64_t tick_count() const;
  uint32_t uptime_ms() const;

 private:
  ButtonState buttons_{};
  uint64_t ticks_ = 0;
  uint32_t uptime_ms_ = 0;

  bool started_ = false;
  bool running_ = true;

  uint32_t inactivity_ms_ = 0;

  bool invert_menu_buttons_ = false;
  bool invert_bottom_paging_ = true;
  bool invert_side_buttons_ = false;
  uint8_t rotate_display_ = 0;  // 0=Portrait(Deg90), 1=Landscape(Deg0), 2=Portrait-Flip(Deg270), 3=Landscape-Flip(Deg180)
  uint8_t rotate_reader_ = 0;   // independent reader rotation, same encoding

  int menu_font_size_ = 2;  // Large default
  uint16_t open_counter_ = 0;  // monotonically increasing; incremented each time a book is opened

  std::string custom_font_path_;
  std::string installed_font_path_;
  std::string sleep_image_path_;  // empty = auto-cycle
  int sleep_image_idx_ = 0;

  ScreenManager screen_mgr_;

  bool show_nav_arrows_ = true;
  bool show_converted_indicator_ = true;
  bool show_reader_images_ = true;
  bool show_sleep_text_ = true;
  uint8_t battery_display_ = 0;  // 0=icon, 1=number, 2=both
  uint8_t list_align_ = 0;       // 0=center, 1=left, 2=right
  uint8_t sleep_timeout_min_ = 10;  // 0=off, else minutes until auto-sleep
  uint8_t menu_theme_ = 4;       // 4=Lyra default

  std::string last_seen_version_;
  bool show_whats_new_on_update_ = true;

  LyraScreen lyra_;
  LyraExtScreen lyra_ext_;
  RecentBooksScreen recent_books_;
  MainMenu menu_;
  ReaderScreen reader_;
  SettingsScreen settings_;
  ReaderOptionsScreen reader_options_;
  ChapterSelectScreen chapter_select_;
  LinksScreen links_screen_;
  ConvertAllScreen convert_all_;
  StatsScreen stats_;
  HiddenBooksMenu hidden_books_;
  WhatsNewScreen whats_new_;

#ifdef MICROREADER_ENABLE_DEMOS
  BouncingBallDemo bouncing_ball_;
  GrayscaleDemo grayscale_demo_;
#endif

  ScreenId pending_push_ = ScreenId::None;
  ScreenId pending_replace_ = ScreenId::None;

  int pending_pop_count_ = 0;

  const BitmapFontSet* reader_font_ = nullptr;
  FontManager* font_manager_ = nullptr;
  std::function<void()> invalidate_font_fn_;

  IScreen* screen_for_(ScreenId id);
};

}  // namespace microreader
