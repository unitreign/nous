#include "ChapterSelectScreen.h"

#include "../Application.h"

namespace microreader {

void ChapterSelectScreen::populate(const TableOfContents& toc, uint16_t current_chapter, uint16_t current_para) {
  entries_.clear();
  entries_.reserve(toc.entries.size());
  initial_selected_ = 0;
  for (const auto& entry : toc.entries) {
    Entry e{};
    e.label = std::string(toc.label_of(entry));
    e.chapter_idx = entry.file_idx;
    e.para_index = entry.para_index;
    e.depth = entry.depth;
    // Select the last TOC entry at or before the current reading position.
    // For earlier chapters any entry qualifies; for the current chapter
    // also compare para_index.
    if (entry.file_idx < current_chapter || (entry.file_idx == current_chapter && entry.para_index <= current_para)) {
      initial_selected_ = static_cast<int>(entries_.size());
    }
    entries_.push_back(e);
  }
}

void ChapterSelectScreen::on_start() {
  set_alignment_left(true);
  title_ = !entries_.empty() ? "Chapters" : "No chapters";
  for (auto& e : entries_)
    add_item(e.label.c_str(), e.depth);
  set_selected(initial_selected_);
}

void ChapterSelectScreen::on_select(int index) {
  pending_chapter_ = entries_[index].chapter_idx;
  pending_para_index_ = entries_[index].para_index;
  has_pending_ = true;
  app_->pop_screen(2);
}

}  // namespace microreader
