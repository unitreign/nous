#include "LinksScreen.h"

#include <cstring>

#include "../Application.h"

namespace microreader {

void LinksScreen::populate(const std::vector<PageLink>& links, const std::vector<std::string>& spine_files,
                           const MrbReader& mrb) {
  entries_.clear();
  entries_.reserve(links.size());
  for (const auto& link : links) {
    Entry e{};
    e.label = link.label;
    e.chapter_idx = 0xFFFF;  // sentinel: no match found
    e.para_idx = 0;

    // Parse "path|fragment" stored in link.href.
    const auto& href = link.href;
    auto sep = href.find('|');
    std::string path_part = (sep != std::string::npos) ? href.substr(0, sep) : href;
    e.fragment = (sep != std::string::npos) ? href.substr(sep + 1) : "";

    // Extract the base filename from path_part (e.g. "OEBPS/Text/ch01.xhtml" → "ch01.xhtml").
    auto slash = path_part.rfind('/');
    std::string basename = (slash != std::string::npos) ? path_part.substr(slash + 1) : path_part;

    // Match against spine_files to find chapter index.
    for (size_t i = 0; i < spine_files.size(); ++i) {
      if (spine_files[i] == basename) {
        e.chapter_idx = static_cast<uint16_t>(i);
        break;
      }
    }

    // Resolve fragment to paragraph index using the anchor table.
    if (e.chapter_idx != 0xFFFF && !e.fragment.empty()) {
      uint16_t para = 0;
      if (mrb.find_anchor(e.chapter_idx, e.fragment.c_str(), e.fragment.size(), para))
        e.para_idx = para;
    }

    entries_.push_back(std::move(e));
  }
}

void LinksScreen::on_start() {
  title_ = !entries_.empty() ? "Links" : "No links";
  for (const auto& e : entries_) {
    std::string display = e.label;
    if (e.chapter_idx == 0xFFFF)
      display += " (?)";
    add_item(display.c_str());
  }
}

void LinksScreen::on_select(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size()))
    return;
  const Entry& e = entries_[index];
  if (e.chapter_idx == 0xFFFF)
    return;  // unresolved link — ignore
  pending_chapter_ = e.chapter_idx;
  pending_para_ = e.para_idx;
  has_pending_ = true;
  // Pop this screen + ReaderOptionsScreen so we return to ReaderScreen.
  // Stack: Reader → ReaderOptions → Links  →  pop(2) → Reader
  app_->pop_screen(2);
}

}  // namespace microreader
