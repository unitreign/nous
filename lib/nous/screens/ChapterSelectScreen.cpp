#include "ChapterSelectScreen.h"

#include "../Application.h"

namespace microreader {

void ChapterSelectScreen::populate(const TableOfContents& toc, uint16_t current_chapter, uint16_t current_para) {
  toc_ = &toc;
  initial_selected_ = 0;
  int i = 0;
  for (const auto& entry : toc.entries) {
    if (entry.file_idx < current_chapter || (entry.file_idx == current_chapter && entry.para_index <= current_para))
      initial_selected_ = i;
    ++i;
  }
}

void ChapterSelectScreen::on_start() {
  set_list_align(1);
  title_ = (toc_ && !toc_->entries.empty()) ? "Chapters" : "No chapters";
  if (toc_) {
    for (const auto& entry : toc_->entries)
      add_item_view(entry.label.view(toc_->pool), entry.depth);
  }
  set_selected(initial_selected_);
}

void ChapterSelectScreen::on_select(int index) {
  pending_chapter_ = toc_->entries[index].file_idx;
  pending_para_index_ = toc_->entries[index].para_index;
  has_pending_ = true;
  app_->pop_screen(2);
}

}  // namespace microreader
