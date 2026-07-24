#include "ChapterSelectScreen.h"

#include <cstdio>

#include "../Application.h"

namespace microreader {

void ChapterSelectScreen::populate(const TableOfContents& toc, uint16_t current_chapter, uint16_t current_para) {
  toc_ = &toc;
  fallback_chapter_count_ = 0;
  initial_selected_ = 0;
  int i = 0;
  for (const auto& entry : toc.entries) {
    if (entry.file_idx < current_chapter || (entry.file_idx == current_chapter && entry.para_index <= current_para))
      initial_selected_ = i;
    ++i;
  }
}

void ChapterSelectScreen::set_chapter_count(uint16_t count, uint16_t current_chapter) {
  toc_ = nullptr;
  fallback_chapter_count_ = count;
  initial_selected_ = (current_chapter < count) ? static_cast<int>(current_chapter) : 0;
}

void ChapterSelectScreen::on_start() {
  set_list_align(1);
  const bool has_toc = toc_ && !toc_->entries.empty();
  if (has_toc) {
    title_ = "Chapters";
    for (const auto& entry : toc_->entries)
      add_item_view(entry.label.view(toc_->pool), entry.depth);
  } else if (fallback_chapter_count_ > 0) {
    title_ = "Chapters";
    char buf[20];
    for (uint16_t i = 0; i < fallback_chapter_count_; ++i) {
      std::snprintf(buf, sizeof(buf), "Chapter %u", static_cast<unsigned>(i + 1));
      add_item(buf);
    }
  } else {
    title_ = "No chapters";
  }
  set_selected(initial_selected_);
}

void ChapterSelectScreen::on_select(int index) {
  if (toc_ && !toc_->entries.empty()) {
    pending_chapter_ = toc_->entries[index].file_idx;
    pending_para_index_ = toc_->entries[index].para_index;
  } else {
    pending_chapter_ = static_cast<uint16_t>(index);
    pending_para_index_ = 0;
  }
  has_pending_ = true;
  app_->pop_screen(2);
}

}  // namespace microreader
