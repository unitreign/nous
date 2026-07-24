#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "../content/ContentModel.h"
#include "ListMenuScreen.h"

namespace microreader {

// Chapter selection screen — lists TOC entries from an MRB file.
// Built on top of ListMenuScreen for consistent UI and scrolling.
// Button3/Button2 = navigate up/down, Button1 = jump to chapter, Button0 = cancel.
class ChapterSelectScreen final : public ListMenuScreen {
 public:
  ChapterSelectScreen() = default;

  // Populate the list from a TableOfContents. Call before pushing this screen.
  // current_chapter/current_para select the closest TOC entry to the reading position.
  void populate(const TableOfContents& toc, uint16_t current_chapter = 0, uint16_t current_para = 0);

  // Fallback: populate by spine chapter count when no TOC entries are available.
  // Generates numbered labels ("Chapter 1", "Chapter 2", …).
  void set_chapter_count(uint16_t count, uint16_t current_chapter = 0);

  const char* name() const override {
    return "Chapters";
  }

  // Returns true if the user selected a chapter (vs. pressing back).
  bool has_pending() const {
    return has_pending_;
  }
  // The chapter index to jump to (valid only when has_pending() == true).
  uint16_t pending_chapter() const {
    return pending_chapter_;
  }
  // The paragraph index within that chapter (0 if no specific anchor).
  uint16_t pending_para_index() const {
    return pending_para_index_;
  }
  void clear_pending() {
    has_pending_ = false;
  }

 protected:
  void on_start() override;
  void on_select(int index) override;
  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override {
    const MenuTheme saved = theme_;
    theme_ = MenuTheme::Minimal;
    ListMenuScreen::draw_all_(buf, battery_pct);
    theme_ = saved;
  }

 private:
  const TableOfContents* toc_ = nullptr;
  int initial_selected_ = 0;
  uint16_t fallback_chapter_count_ = 0;  // used when toc_ has no entries

  uint16_t pending_chapter_ = 0;
  uint16_t pending_para_index_ = 0;
  bool has_pending_ = false;
};

}  // namespace microreader
