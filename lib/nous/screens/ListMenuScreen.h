#pragma once

#include <cstdint>
#include <deque>
#include <string_view>
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
  // 0 = small (14px), 1 = medium (18px), 2 = large (24px)
  static void set_font_size(int size) {
    font_size_idx_ = size;
  }
  static int font_size() {
    return font_size_idx_;
  }

  // Global visual theme — affects all ListMenuScreen instances (static).
  enum class MenuTheme : uint8_t { Chronicle = 0, Minimal = 1, Stele = 2, Codex = 3, Lyra = 4 };
  static void set_theme(MenuTheme t) { theme_ = t; }
  static MenuTheme theme() { return theme_; }

 protected:
  const char* title_ = nullptr;
  const char* title2_ = nullptr;

  std::string subtitle_;
  std::string subtitle2_;
  std::string subtitle3_;

  // 0 = center (default), 1 = left, 2 = right
  void set_list_align(uint8_t align) {
    list_align_ = align;
  }
  uint8_t list_align() const {
    return list_align_;
  }

  void add_item(const std::string& label, int indent = 0) {
    owned_strings_.push_back(label);
    labels_.push_back(std::string_view(owned_strings_.back()));
    separators_.push_back(false);
    indents_.push_back(indent);
  }
  // Zero-copy overload: stores a view into the caller-owned string.
  // The caller must ensure the referenced string outlives this screen.
  void add_item_view(std::string_view label, int indent = 0) {
    labels_.push_back(label);
    separators_.push_back(false);
    indents_.push_back(indent);
  }
  // Insert a visual separator (thin horizontal line, non-selectable).
  void add_separator(const std::string& header = "") {
    owned_strings_.push_back(header);
    labels_.push_back(std::string_view(owned_strings_.back()));
    separators_.push_back(true);
    indents_.push_back(0);
  }
  void set_item_label(int index, const std::string& label) {
    if (index >= 0 && index < static_cast<int>(labels_.size())) {
      owned_strings_.push_back(label);
      labels_[index] = std::string_view(owned_strings_.back());
    }
  }
  void clear_items() {
    labels_.clear();
    owned_strings_.clear();
    separators_.clear();
    indents_.clear();
    selected_ = 0;
    scroll_offset_ = 0;
  }
  // Free all item storage without touching selected_/scroll_offset_.
  // Call from stop() to release RAM while preserving the cursor position
  // for the fallback path in ListMenuScreen::start().
  void free_items_storage() {
    { std::vector<std::string_view> tmp; labels_.swap(tmp); }
    { std::deque<std::string> tmp; owned_strings_.swap(tmp); }
    { std::vector<bool> tmp; separators_.swap(tmp); }
    { std::vector<int> tmp; indents_.swap(tmp); }
  }
  int selected() const { return selected_; }
  int scroll_offset() const { return scroll_offset_; }
  void set_selected(int index) {
    selected_ = index;
    on_start_set_selection_ = true;
  }
  virtual bool is_separator(int index) const {
    return index >= 0 && index < static_cast<int>(separators_.size()) && separators_[index];
  }
  // Returns true if the cursor may land on this item. Default: not a separator.
  // Override to additionally exclude theme-irrelevant items.
  virtual bool is_item_focusable(int index) const { return !is_separator(index); }
  // Returns true if this item represents a converted book (Stele divider width).
  // Default: false. Override in MainMenu.
  virtual bool is_item_converted(int index) const { return false; }
  virtual int count() const {
    return static_cast<int>(labels_.size());
  }

  // Label for item at index. Default reads from labels_[]; override to provide
  // labels dynamically without populating the labels_ vector.
  virtual std::string_view get_item_label(int index) const {
    if (index >= 0 && index < static_cast<int>(labels_.size()))
      return labels_[index];
    return {};
  }

  // Nous theme: secondary line below the item label (e.g. author, read time, setting value).
  // Default returns empty (no subtitle). Override per screen.
  virtual std::string_view get_item_subtitle(int index) const { return {}; }

  // Chronicle theme: right-aligned text on the title line (e.g. read time, "–").
  // Default returns empty (no right column). Override per screen.
  virtual std::string_view get_item_right(int index) const { return {}; }

  // Nous theme: left side of the top bar header (e.g. "X books", "Settings").
  // Default returns title_ if set. Override per screen.
  virtual std::string nous_header_left() const { return title_ ? title_ : ""; }

  // Nous theme: section title drawn in header_font_ below the status bar.
  // Empty = no section title row. Override in screens that need a page heading.
  virtual std::string nous_section_title() const { return {}; }

  // Called during start(). Set title_ and call add_item() to populate the list.
  virtual void on_start() = 0;

  // Called when user presses select on an item.
  virtual void on_select(int index) = 0;

  // Called when user presses back.
  virtual void on_back();

 protected:
  BitmapFont ui_font_;
  BitmapFont header_font_;
  BitmapFont subtitle_font_;   // always small; used for item subtitles and tight labels
  BitmapFont section_font_;    // one step below ui_font_; use for APPEARANCE/NAVIGATE etc.
  static int font_size_idx_;  // 0=Normal, 1=Large, 2=XLarge
  static MenuTheme theme_;

  void request_redraw() {
    force_redraw_ = true;
  }

  // Re-run start() to rebuild items with updated settings (e.g. after font change).
  void restart() {
    if (buf_ && runtime_)
      start(*buf_, *runtime_);
  }

  virtual void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const;
  void ensure_visible_();
  void center_on_selected_();

  // Returns the number of visual indices visible from scroll_off given screen height H.
  // Override in subclasses whose draw_all_() uses a custom header or item height.
  virtual int get_visible_count_(int H, int scroll_off) const;

 private:
  std::vector<std::string_view> labels_;
  std::deque<std::string> owned_strings_;  // backing storage for copied labels
  std::vector<bool> separators_;
  std::vector<int> indents_;

  int selected_ = 0;
  int scroll_offset_ = 0;
  int initial_selection_ = -1;
  int hold_frames_up_ = 0;
  int hold_frames_down_ = 0;

  uint8_t list_align_ = 0;  // 0=center, 1=left, 2=right
  bool on_start_set_selection_ = false;
  bool force_redraw_ = false;

  DrawBuffer* buf_ = nullptr;
  IRuntime* runtime_ = nullptr;

  // Computes the header height (title + subtitles) without drawing anything.
  // Used by ensure_visible_() and center_on_selected_() before a draw pass.
  int compute_header_h_() const;

  // 1. Draws title, title2, and subtitles. Returns the header height (pixels from y=0 to
  //    where list items may start).
  int draw_header_(DrawBuffer& buf, int W, int H, std::optional<uint8_t> battery_pct = {}) const;

  // Nous theme: height of one list item slot (title + subtitle + padding + divider).
  int nous_slot_h_() const;

  // Nous theme: how many items are visible starting at scroll_off given available_h pixels.
  // Accounts for separator items being shorter than regular slots.
  int nous_visible_from_(int scroll_off, int available_h) const;

  // 2. Draws the battery bar and button-hint glyphs at the bottom of the screen.
  //    Returns the height reserved at the bottom (list items must stay above this).
  int draw_bottom_(DrawBuffer& buf, int W, int H, std::optional<uint8_t> battery_pct) const;

  // 3. Draws the scrollable item list, scrollbar, and area-boundary indicator lines,
  //    given the already-known header and bottom heights.
  void draw_list_(DrawBuffer& buf, int W, int H, int header_h, int bottom_h) const;

  // 4. Draws the four navigaton-button hint glyphs. Called by draw_bottom_.
  void draw_button_hints_(DrawBuffer& buf) const;
};

}  // namespace microreader
