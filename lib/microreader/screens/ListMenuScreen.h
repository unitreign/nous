#pragma once

#include <cstdint>
#include <vector>

#include "../Input.h"
#include "../display/DrawBuffer.h"
#include "IScreen.h"

namespace microreader {

// Base class for screens that show a titled list of selectable items.
// Handles drawing (header font for title, UI font for items with selection bar),
// up/down navigation with wrapping, scrolling for long lists, and font
// initialization from embedded data.
//
// Subclasses implement:
//   on_start()      — set title, populate items via add_item()
//   on_select(index) — handle item selection; return true to stay, false to exit
//   on_back()       — handle back button; return true to stay, false to exit (default)
class ListMenuScreen : public IScreen {
 public:
  void start(DrawBuffer& buf, IRuntime& runtime) override;
  void stop() override {}
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

  int selected_index() const {
    return selected_;
  }
  void set_initial_selection(int index) {
    initial_selection_ = index;
  }

  // Global font size — affects all ListMenuScreen instances (static).
  // 0 = Normal (14px), 1 = Large (18px), 2 = XLarge (22px)
  static void set_font_size(int size) {
    font_size_idx_ = size;
  }
  static int font_size() {
    return font_size_idx_;
  }

 protected:
  const char* title_ = nullptr;
  std::string subtitle_;
  std::string subtitle2_;
  std::string subtitle3_;

  void set_alignment_left(bool left) {
    align_left_ = left;
  }

  void add_item(const std::string& label, int indent = 0) {
    labels_.push_back(label);
    separators_.push_back(false);
    indents_.push_back(indent);
  }
  // Insert a visual separator (thin horizontal line, non-selectable).
  void add_separator() {
    labels_.push_back("");
    separators_.push_back(true);
    indents_.push_back(0);
  }
  void set_item_label(int index, const std::string& label) {
    if (index >= 0 && index < static_cast<int>(labels_.size())) {
      labels_[index] = label;
    }
  }
  void clear_items() {
    labels_.clear();
    separators_.clear();
    indents_.clear();
    selected_ = 0;
    scroll_offset_ = 0;
  }
  int selected() const {
    return selected_;
  }
  void set_selected(int index) {
    selected_ = index;
    on_start_set_selection_ = true;
  }
  int count() const {
    return static_cast<int>(labels_.size());
  }

  // Called during start(). Set title_ and call add_item() to populate the list.
  virtual void on_start() = 0;

  // Called when user presses select on an item.
  virtual void on_select(int index) = 0;

  // Called when user presses back.
  virtual void on_back();

 private:
  std::vector<std::string> labels_;
  std::vector<bool> separators_;
  std::vector<int> indents_;
  bool align_left_ = false;
  int selected_ = 0;
  int scroll_offset_ = 0;
  bool on_start_set_selection_ = false;
  int initial_selection_ = -1;
  // Hold-down acceleration counters (frames button has been held without a fresh press).
  int hold_frames_up_ = 0;
  int hold_frames_down_ = 0;

  BitmapFont ui_font_;
  static int font_size_idx_;  // 0=Normal, 1=Large, 2=XLarge
  BitmapFont header_font_;
  DrawBuffer* buf_ = nullptr;
  IRuntime* runtime_ = nullptr;

 protected:
  // Re-run start() to rebuild items with updated settings (e.g. after font change).
  void restart() {
    if (buf_ && runtime_)
      start(*buf_, *runtime_);
  }

  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const;
  void ensure_visible_();
  void center_on_selected_();

 private:
  void draw_button_hints_(DrawBuffer& buf) const;
};

}  // namespace microreader
