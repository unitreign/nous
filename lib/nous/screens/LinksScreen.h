#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../content/mrb/MrbReader.h"
#include "ListMenuScreen.h"
#include "ReaderOptionsScreen.h"  // for PageLink

namespace microreader {

// Links screen — lists hyperlinks found on the current reader page.
// User selects a link to navigate to the target chapter.
// Built on top of ListMenuScreen for consistent UI and scrolling.
// Button3/Button2 = navigate up/down, Button1 = go to link, Button0 = cancel.
class LinksScreen final : public ListMenuScreen {
 public:
  LinksScreen() = default;

  const char* name() const override {
    return "Links";
  }

  // Populate from the links on the current page.
  // spine_files maps chapter index → base filename (from MrbReader::spine_files()).
  // mrb is used to resolve fragment anchors to paragraph indices.
  // Call before pushing this screen.
  void populate(const std::vector<PageLink>& links, const std::vector<std::string>& spine_files, const MrbReader& mrb);

  // Returns true if the user selected a link.
  bool has_pending() const {
    return has_pending_;
  }
  // The chapter index to jump to (valid only when has_pending() == true).
  uint16_t pending_chapter() const {
    return pending_chapter_;
  }
  // The paragraph index within that chapter (0 — fragment resolution not implemented yet).
  uint16_t pending_para() const {
    return pending_para_;
  }
  void clear_pending() {
    has_pending_ = false;
  }

  // Test helper: simulate the user selecting entry at index.
  // Safe to call with a null or default-constructed Application (pop_screen is deferred).
  void test_select(int index) {
    on_select(index);
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
  struct Entry {
    std::string label;
    std::string fragment;  // anchor id within the target chapter (may be empty)
    uint16_t chapter_idx;
    uint16_t para_idx = 0;  // paragraph index within chapter (0 = start)
  };
  std::vector<Entry> entries_;

  uint16_t pending_chapter_ = 0;
  uint16_t pending_para_ = 0;
  bool has_pending_ = false;
};

}  // namespace microreader
