#include "ReaderOptionsScreen.h"

#include <climits>
#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../content/BookIndex.h"

namespace microreader {
constexpr uint16_t ReaderSettings::kHPaddingPresets[];
constexpr uint16_t ReaderSettings::kVPaddingPresets[];
constexpr uint16_t ReaderSettings::kSpacingPercents[];
constexpr uint8_t ReaderSettings::kNumAlignPresets;
constexpr uint8_t ReaderSettings::kNumSpacingPresets;
constexpr uint8_t ReaderSettings::kNumFontSizePresets;
constexpr const char* ReaderSettings::kHPaddingNames[];
constexpr const char* ReaderSettings::kVPaddingNames[];
constexpr const char* ReaderSettings::kAlignNames[];
constexpr const char* ReaderSettings::kSpacingNames[];
constexpr const char* ReaderSettings::kFontSizeNames[];

// ---------------------------------------------------------------------------

void ReaderOptionsScreen::populate(const TableOfContents& toc, uint16_t current_chapter, uint16_t current_para,
                                   const std::string& fallback_title, int book_progress_pct, int chapter_progress_pct) {
  toc_ = &toc;
  book_title_ = fallback_title;
  chapter_title_ = fallback_title;
  int best_match = -1;
  for (size_t i = 0; i < toc_->entries.size(); ++i) {
    if (toc_->entries[i].file_idx < current_chapter ||
        (toc_->entries[i].file_idx == current_chapter && toc_->entries[i].para_index <= current_para)) {
      best_match = static_cast<int>(i);
    }
  }
  if (best_match >= 0) {
    chapter_title_ = toc_->entries[best_match].label.to_string(toc_->pool);
  }

  book_progress_pct_ = book_progress_pct;
  chapter_progress_pct_ = chapter_progress_pct;
  if (toc_ && !toc_->entries.empty() && app_) {
    app_->chapter_select()->populate(*toc_, current_chapter, current_para);
    app_->chapter_select()->clear_pending();
  }
}

// Build a "Label: Value" string into a fixed buffer.
static const char* fmt_setting(char* buf, size_t bufsz, const char* label, const char* value) {
  snprintf(buf, bufsz, "%s: %s", label, value);
  return buf;
}

std::string_view ReaderOptionsScreen::get_item_subtitle(int index) const {
  std::string_view label = ListMenuScreen::get_item_label(index);
  const auto pos = label.find(": ");
  if (pos == std::string_view::npos) return {};
  subtitle_buf_ = label.substr(pos + 2);
  return subtitle_buf_;
}

std::string ReaderOptionsScreen::nous_header_left() const {
  return "reading";
}

void ReaderOptionsScreen::start(DrawBuffer& buf, IRuntime& runtime) {
  buf_ = &buf;
  // Capture current selection and Links position before the base class
  // calls on_start(), which rebuilds the list.
  prev_selected_ = selected_index();
  prev_idx_links_ = idx_links_;
  ListMenuScreen::start(buf, runtime);
}

void ReaderOptionsScreen::set_page_links(const std::vector<PageLink>& links,
                                         const std::vector<std::string>& spine_files, const MrbReader& mrb) {
  page_links_ = links;
  // Populate the links screen now while the MrbReader is still open.
  // (ReaderScreen::stop() will close mrb before we get to on_select.)
  if (app_)
    app_->links_screen()->populate(links, spine_files, mrb);
}

void ReaderOptionsScreen::on_start() {
  title2_ = nullptr;
  book_title1_buf_.clear();

  if (header_font_.valid()) {
    const int max_line_w = buf_->width() - 24;
    const int total_w = header_font_.word_width(book_title_.c_str(), book_title_.length(), FontStyle::Regular);
    if (total_w <= max_line_w) {
      title_ = book_title_.c_str();
    } else {
      std::vector<std::string> words;
      std::string w;
      for (size_t i = 0; i <= book_title_.length(); ++i) {
        if (i == book_title_.length() || book_title_[i] == ' ') {
          if (!w.empty())
            words.push_back(w);
          w.clear();
        } else {
          w += book_title_[i];
        }
      }

      std::vector<int> word_w;
      word_w.reserve(words.size());
      int all_w = 0;
      for (const auto& word : words) {
        int ww = static_cast<int>(header_font_.word_width(word.c_str(), word.length(), FontStyle::Regular));
        word_w.push_back(ww);
        all_w += ww;
      }
      int spaces = static_cast<int>(words.size()) - 1;
      if (spaces > 0)
        all_w += spaces * static_cast<int>(header_font_.word_width(" ", 1, FontStyle::Regular));

      if (all_w <= max_line_w) {
        title_ = book_title_.c_str();
      } else {
        int space_w = static_cast<int>(header_font_.word_width(" ", 1, FontStyle::Regular));
        int ellipsis_w = static_cast<int>(header_font_.word_width("...", 3, FontStyle::Regular));

        int best_split = 1;
        int best_diff = INT_MAX;
        int l1_w = 0;
        for (size_t i = 0; i < words.size(); ++i) {
          if (i > 0)
            l1_w += space_w;
          l1_w += word_w[i];
          if (l1_w > max_line_w)
            break;
          if (i + 1 < words.size()) {
            int l2_w = 0;
            for (size_t j = i + 1; j < words.size(); ++j) {
              if (j > i + 1)
                l2_w += space_w;
              l2_w += word_w[j];
            }
            int diff = std::abs(l1_w - l2_w);
            if (diff < best_diff) {
              best_diff = diff;
              best_split = static_cast<int>(i) + 1;
            }
          }
        }

        std::string line1, line2;
        for (int i = 0; i < best_split; ++i) {
          if (i > 0)
            line1 += ' ';
          line1 += words[i];
        }
        for (int i = best_split; i < static_cast<int>(words.size()); ++i) {
          if (i > best_split)
            line2 += ' ';
          line2 += words[i];
        }

        if (header_font_.word_width(line2.c_str(), line2.length(), FontStyle::Regular) <= max_line_w) {
          book_title2_buf_ = line2;
          title2_ = book_title2_buf_.c_str();
        } else {
          int l2_w = 0;
          int trunc_at = 0;
          for (int i = best_split; i < static_cast<int>(words.size()); ++i) {
            int word_with_space = word_w[i] + ((i > best_split) ? space_w : 0);
            if (l2_w + word_with_space + ellipsis_w > max_line_w)
              break;
            l2_w += word_with_space;
            trunc_at = i + 1;
          }
          line2.clear();
          for (int i = best_split; i < trunc_at; ++i) {
            if (i > best_split)
              line2 += ' ';
            line2 += words[i];
          }
          line2 += "...";
          book_title2_buf_ = line2;
          title2_ = book_title2_buf_.c_str();
        }

        book_title1_buf_ = line1;
        title_ = book_title1_buf_.c_str();
      }
    }
  } else {
    if (book_title_.length() > 30) {
      book_title1_buf_ = book_title_.substr(0, 27) + "...";
      title_ = book_title1_buf_.c_str();
    } else {
      title_ = book_title_.c_str();
    }
  }

  // Look up the author in one BookIndex pass.
  //
  // The read time comes from the reader alone. reading_ms_total() is already
  // the lifetime total (persisted .pos value + the live session), and the
  // index entry is a copy of that same total written by the previous stop() —
  // adding them displayed roughly double. The equivalent bug was fixed on the
  // Stats path in 6cad8c9 but missed here.
  subtitle3_ = "< 1m read";
  std::string_view author_sv;
  if (app_ && app_->reader()) {
    const uint64_t ms = app_->reader()->reading_ms_total();
    const std::string cur_path = app_->reader()->get_path();
    for (const auto& e : BookIndex::instance().entries()) {
      if (e.path.view(BookIndex::instance().pool()) == cur_path) {
        author_sv = e.author.view(BookIndex::instance().pool());
        break;
      }
    }
    const uint64_t total_min = ms / 60000;
    if (total_min >= 60) {
      const unsigned hours = static_cast<unsigned>(total_min / 60);
      const unsigned mins  = static_cast<unsigned>(total_min % 60);
      char tbuf[20];
      std::snprintf(tbuf, sizeof(tbuf), "%uh %um read", hours, mins);
      subtitle3_ = tbuf;
    } else if (total_min >= 1) {
      char tbuf[12];
      std::snprintf(tbuf, sizeof(tbuf), "%um read", static_cast<unsigned>(total_min));
      subtitle3_ = tbuf;
    }
  }

  // Book-details card (unified across all themes): author, book%, read time.
  subtitle_ = std::string(author_sv);
  char pct_buf[16];
  snprintf(pct_buf, sizeof(pct_buf), "Book %d%%", book_progress_pct_);
  subtitle2_ = pct_buf;
  // subtitle3_ stays as the read time string

  clear_items();
  idx_justify_ = idx_padding_h_ = idx_padding_v_ = idx_line_spacing_ = idx_progress_ = idx_progress_scope_ =
      idx_chapters_ = idx_pub_fonts_ = idx_hyphenation_ = idx_rotate_display_ = idx_reader_images_ = idx_links_ = idx_stats_ = -1;

  char tmp[40];

  idx_stats_ = count();
  add_item("Statistics");

  bool has_toc = toc_ && !toc_->entries.empty();
  const bool has_nav = has_toc || !page_links_.empty();

  if (has_nav) {
    add_separator("NAVIGATE");
    if (app_ && has_toc) {
      idx_chapters_ = count();
      add_item("Chapters");
    }
    if (!page_links_.empty()) {
      idx_links_ = count();
      char link_label[40];
      snprintf(link_label, sizeof(link_label), "Links (%d)", static_cast<int>(page_links_.size()));
      add_item(link_label);
    }
  }

  if (settings_) {
    add_separator("SETTINGS");
  }

  if (settings_) {
    idx_font_size_ = count();
    if (app_ && app_->font_manager() && app_->font_manager()->valid()) {
      auto* fonts = app_->font_manager()->font_set();
      int sz = 0;
      if (fonts && settings_->font_size_idx < fonts->num_fonts()) {
        auto* f = fonts->get_font(settings_->font_size_idx);
        if (f) {
          sz = f->nominal_size();
          if (sz == 0)
            sz = f->y_advance();
        }
      }
      char val[16];
      if (sz > 0)
        snprintf(val, sizeof(val), "%d", sz);
      else
        snprintf(val, sizeof(val), "Unknown");
      add_item(fmt_setting(tmp, sizeof(tmp), "Font Size", val));
    } else {
      add_item(fmt_setting(tmp, sizeof(tmp), "Font Size", ReaderSettings::kFontSizeNames[settings_->font_size_idx]));
    }

    idx_pub_fonts_ = count();
    add_item(fmt_setting(tmp, sizeof(tmp), "Publisher Sizes", settings_->override_publisher_fonts ? "Off" : "On"));

    idx_line_spacing_ = count();
    add_item(fmt_setting(tmp, sizeof(tmp), "Line spacing",
                         ReaderSettings::kSpacingNames[static_cast<uint8_t>(settings_->spacing_override)]));

    idx_justify_ = count();
    add_item(fmt_setting(tmp, sizeof(tmp), "Alignment",
                         ReaderSettings::kAlignNames[static_cast<uint8_t>(settings_->align_override)]));

    idx_hyphenation_ = count();
    add_item(fmt_setting(tmp, sizeof(tmp), "Hyphenation", settings_->hyphenation_enabled ? "On" : "Off"));

    add_separator();

    idx_padding_h_ = count();
    add_item(fmt_setting(tmp, sizeof(tmp), "H-Margin", ReaderSettings::kHPaddingNames[settings_->padding_h_idx]));

    idx_padding_v_ = count();
    add_item(fmt_setting(tmp, sizeof(tmp), "V-Margin", ReaderSettings::kVPaddingNames[settings_->padding_v_idx]));

    add_separator();

    idx_progress_ = count();
    const char* prog_name = settings_->progress_style == ProgressStyle::None         ? "None"
                            : settings_->progress_style == ProgressStyle::Percentage ? "Percent"
                                                                                     : "Bar";
    add_item(fmt_setting(tmp, sizeof(tmp), "Progress", prog_name));

    if (settings_->progress_style != ProgressStyle::None) {
      idx_progress_scope_ = count();
      add_item(fmt_setting(tmp, sizeof(tmp), "Progress Scope",
                           settings_->progress_scope == ProgressScope::Chapter ? "Chapter" : "Book"));
    }

    idx_reader_images_ = count();
    add_item(fmt_setting(tmp, sizeof(tmp), "Images", (app_ && !app_->show_reader_images()) ? "Off" : "On"));

    idx_reader_rotate_display_ = count();
    add_item(fmt_setting(tmp, sizeof(tmp), "Reader Display", rotation_label(app_ ? app_->rotate_reader() : 0)));
  }

  // Restore selection, adjusting for Links appearing or disappearing.
  int sel = prev_selected_;
  if (prev_idx_links_ == -1 && idx_links_ != -1) {
    // Links appeared: everything at idx_links_ and below shifted down by 1.
    if (sel >= idx_links_)
      sel++;
  } else if (prev_idx_links_ != -1 && idx_links_ == -1) {
    // Links disappeared: cursor was on Links or below it — move up by 1.
    if (sel >= prev_idx_links_)
      sel--;
  }
  int max_sel = count() - 1;
  if (max_sel >= 0) {
    if (sel < 0)
      sel = 0;
    if (sel > max_sel)
      sel = max_sel;
  }
  set_selected(sel);
}

void ReaderOptionsScreen::refresh_items_(int restore_selection) {
  prev_selected_ = restore_selection;
  prev_idx_links_ = idx_links_;
  on_start();  // on_start() applies shift correction and calls set_selected().
}

int ReaderOptionsScreen::get_visible_count_(int H, int scroll_off) const {
  int list_top = 16;
  if (title_ && header_font_.valid()) {
    list_top += header_font_.y_advance();
    if (title2_) list_top += header_font_.y_advance();
  } else if (title_) {
    list_top += ui_font_.y_advance();
  }
  list_top += 4;
  if (!subtitle_.empty())      list_top += ui_font_.y_advance() + 3;
  if (!chapter_title_.empty()) list_top += ui_font_.y_advance() + 3;
  list_top += ui_font_.y_advance() + 10 + 1;  // stats row + rule
  const int available_h = H - list_top;
  int h = 0, cnt = 0, n = count();
  for (int i = scroll_off; i < n; ++i) {
    const int item_h = is_separator(i)
        ? (8 + (!get_item_label(i).empty() && section_font_.valid() ? section_font_.y_advance() : 0) + 4)
        : kRowH;
    if (h + item_h > available_h) break;
    h += item_h;
    cnt++;
  }
  return cnt;
}

void ReaderOptionsScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct) const {
  const int W = buf.width();
  const int H = buf.height();
  buf.fill(true);

  if (!ui_font_.valid() || !subtitle_font_.valid()) return;

  static constexpr int kLM = 14, kRM = 14;
  int y = 16;

  // ── Book title ──────────────────────────────────────────────────────────
  if (title_ && header_font_.valid()) {
    buf.draw_text_proportional(kLM, y + header_font_.baseline(), title_, header_font_, false);
    y += header_font_.y_advance();
    if (title2_) {
      buf.draw_text_proportional(kLM, y + header_font_.baseline(), title2_, header_font_, false);
      y += header_font_.y_advance();
    }
  } else if (title_) {
    buf.draw_text_proportional(kLM, y + ui_font_.baseline(), title_, ui_font_, false);
    y += ui_font_.y_advance();
  }
  y += 4;

  // ── Author ──────────────────────────────────────────────────────────────
  if (!subtitle_.empty()) {
    buf.draw_text_proportional(kLM, y + ui_font_.baseline(),
                               subtitle_.c_str(), subtitle_.size(), ui_font_, false);
    y += ui_font_.y_advance() + 3;
  }

  // ── Chapter name ────────────────────────────────────────────────────────
  if (!chapter_title_.empty()) {
    static const char kEll[] = "...";
    const int max_cw = W - kLM - kRM;
    const int ell_w = ui_font_.word_width(kEll, 3, FontStyle::Regular);
    const char* cp = chapter_title_.c_str();
    const size_t clen = chapter_title_.size();
    if (ui_font_.word_width(cp, clen, FontStyle::Regular) > max_cw) {
      const int budget = max_cw - ell_w;
      size_t fit = 0;
      const char* p = cp;
      while (*p) {
        const uint8_t b = static_cast<uint8_t>(*p);
        const size_t cb = b < 0x80 ? 1u : b < 0xE0 ? 2u : b < 0xF0 ? 3u : 4u;
        if (ui_font_.word_width(cp, fit + cb, FontStyle::Regular) > budget) break;
        fit += cb; p += cb;
      }
      subtitle_buf_.assign(cp, fit);
      subtitle_buf_ += kEll;
      buf.draw_text_proportional(kLM, y + ui_font_.baseline(),
                                 subtitle_buf_.c_str(), subtitle_buf_.size(), ui_font_, false);
    } else {
      buf.draw_text_proportional(kLM, y + ui_font_.baseline(), cp, clen, ui_font_, false);
    }
    y += ui_font_.y_advance() + 3;
  }

  // ── Stats: "Book X% · Chapter Y%" left, read time right ────────────────
  {
    char stats_l[48];
    snprintf(stats_l, sizeof(stats_l), "Book %d%%  \xc2\xb7  Chapter %d%%",
             book_progress_pct_, chapter_progress_pct_);
    buf.draw_text_proportional(kLM, y + ui_font_.baseline(),
                               stats_l, std::strlen(stats_l), ui_font_, false);
    if (!subtitle3_.empty()) {
      const int rw = ui_font_.word_width(subtitle3_.c_str(), subtitle3_.size(), FontStyle::Regular);
      buf.draw_text_proportional(W - kRM - rw, y + ui_font_.baseline(),
                                 subtitle3_.c_str(), subtitle3_.size(), ui_font_, false);
    }
    y += ui_font_.y_advance() + 10;
  }

  buf.fill_rect(0, y, W, 1, false);
  y += 1;

  // ── Item list ────────────────────────────────────────────────────────────
  const int n = count();
  // Compute total content height to decide whether scrolling is needed.
  int total_h = 0;
  for (int i = 0; i < n; ++i) {
    if (is_separator(i))
      total_h += 8 + (!get_item_label(i).empty() && section_font_.valid() ? section_font_.y_advance() : 0) + 4;
    else
      total_h += kRowH;
  }
  const int so = (total_h <= H - y) ? 0 : scroll_offset();

  for (int i = so; i < n && y < H; ++i) {
    const bool sel = (i == selected());

    if (is_separator(i)) {
      const std::string_view hdr = get_item_label(i);
      y += 8;
      if (!hdr.empty()) {
        buf.draw_text_proportional(kLM, y + section_font_.baseline(),
                                   hdr.data(), hdr.size(), section_font_, false);
        y += section_font_.y_advance();
      }
      y += 4;
      continue;
    }

    const std::string_view label = get_item_label(i);
    std::string_view display_label = label;
    std::string_view display_value;
    const auto pos = label.find(": ");
    if (pos != std::string_view::npos) {
      display_label = label.substr(0, pos);
      display_value = label.substr(pos + 2);
    }

    const bool is_nav = (i == idx_chapters_ || i == idx_links_);

    if (sel)
      buf.fill_rect(0, y, W, kRowH, false);

    const int text_y = y + (kRowH - ui_font_.y_advance()) / 2 + ui_font_.baseline();
    buf.draw_text_proportional(kLM, text_y, display_label.data(), display_label.size(), ui_font_, sel);

    if (is_nav) {
      static const char kArrow[] = ">";
      const int aw = ui_font_.word_width(kArrow, 1, FontStyle::Regular);
      buf.draw_text_proportional(W - kRM - aw, text_y, kArrow, 1, ui_font_, sel);
    } else if (!display_value.empty()) {
      const int vw = ui_font_.word_width(display_value.data(), display_value.size(), FontStyle::Regular);
      buf.draw_text_proportional(W - kRM - vw, text_y, display_value.data(), display_value.size(), ui_font_, sel);
    }

    y += kRowH;
  }
}

void ReaderOptionsScreen::on_select(int index) {
  if (!settings_) {
    if (index == idx_chapters_) {
      app_->push_screen(ScreenId::ChapterSelect);
      return;
    }
    return;
  }

  if (index == idx_justify_) {
    settings_->align_override = static_cast<AlignOverride>((static_cast<uint8_t>(settings_->align_override) + 1) %
                                                           ReaderSettings::kNumAlignPresets);
    refresh_items_(index);
    return;
  }
  if (index == idx_hyphenation_) {
    settings_->hyphenation_enabled = !settings_->hyphenation_enabled;
    refresh_items_(index);
    return;
  }
  if (index == idx_pub_fonts_) {
    settings_->override_publisher_fonts = !settings_->override_publisher_fonts;
    refresh_items_(index);
    return;
  }
  if (index == idx_font_size_) {
    uint8_t max_idx = ReaderSettings::kNumFontSizePresets;
    if (app_ && app_->font_manager() && app_->font_manager()->valid()) {
      auto* fonts = app_->font_manager()->font_set();
      if (fonts && fonts->num_fonts() > 0)
        max_idx = static_cast<uint8_t>(fonts->num_fonts());
    }
    settings_->font_size_idx = static_cast<uint8_t>((settings_->font_size_idx + 1) % max_idx);
    refresh_items_(index);
    return;
  }
  if (index == idx_padding_h_) {
    settings_->padding_h_idx = static_cast<uint8_t>((settings_->padding_h_idx + 1) % ReaderSettings::kNumPresets);
    refresh_items_(index);
    return;
  }
  if (index == idx_padding_v_) {
    settings_->padding_v_idx = static_cast<uint8_t>((settings_->padding_v_idx + 1) % ReaderSettings::kNumPresets);
    refresh_items_(index);
    return;
  }
  if (index == idx_line_spacing_) {
    settings_->spacing_override = static_cast<SpacingOverride>((static_cast<uint8_t>(settings_->spacing_override) + 1) %
                                                               ReaderSettings::kNumSpacingPresets);
    refresh_items_(index);
    return;
  }
  if (index == idx_progress_) {
    settings_->progress_style = static_cast<ProgressStyle>((static_cast<uint8_t>(settings_->progress_style) + 1) % 3);
    refresh_items_(index);
    return;
  }
  if (index == idx_progress_scope_) {
    settings_->progress_scope =
        settings_->progress_scope == ProgressScope::Book ? ProgressScope::Chapter : ProgressScope::Book;
    refresh_items_(index);
    return;
  }
  if (index == idx_reader_images_) {
    if (app_) {
      app_->set_show_reader_images(!app_->show_reader_images());
      refresh_items_(index);
    }
    return;
  }
  if (index == idx_reader_rotate_display_) {
    if (app_) {
      uint8_t v = static_cast<uint8_t>((app_->rotate_reader() + 1) % 4);
      app_->set_rotate_reader(v);
      refresh_items_(index);
    }
    return;
  }
  if (index == idx_chapters_) {
    app_->push_screen(ScreenId::ChapterSelect);
    return;
  }
  if (index == idx_links_) {
    app_->push_screen(ScreenId::Links);
    return;
  }
  if (index == idx_stats_) {
    if (app_) {
      auto* r = app_->reader();
      app_->stats_screen()->set_book_stats(
          r->book_title(), subtitle_, chapter_title_,
          static_cast<int>(r->chapter_index()), static_cast<int>(r->chapter_count()),
          r->times_opened(), r->reading_ms_total(),
          r->progress_pct(), chapter_progress_pct_,
          r->estimated_time_left_ms(), r->page_turn_count(),
          r->get_path());
      app_->push_screen(ScreenId::Stats);
    }
    return;
  }
  return;
}

}  // namespace microreader
