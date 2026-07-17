#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "StringPool.h"

namespace microreader {

// ---------------------------------------------------------------------------
// Font style
// ---------------------------------------------------------------------------

enum class FontStyle : uint8_t {
  Regular = 0,
  Bold = 1,
  Italic = 2,
  BoldItalic = 3,
};

// ---------------------------------------------------------------------------
// Font size limit
// ---------------------------------------------------------------------------

// The maximum number of dynamically scaled fonts we keep loaded simultaneously.
static constexpr int kMaxFontSizes = 8;

// ---------------------------------------------------------------------------
// Text alignment
// ---------------------------------------------------------------------------

enum class Alignment : uint8_t {
  Start = 0,
  Center,
  End,
  Justify,
};

enum class VerticalAlign : uint8_t {
  Baseline = 0,
  Super,
  Sub,
};

// ---------------------------------------------------------------------------
// A "run" of text with uniform style (matches TrustyReader's layout::Run)
// ---------------------------------------------------------------------------

struct Run {
  std::string text;
  FontStyle style = FontStyle::Regular;
  uint8_t size_pct = 100;
  VerticalAlign vertical_align = VerticalAlign::Baseline;
  bool breaking = false;      // true for <br> line breaks
  uint16_t margin_left = 0;   // inline left margin in pixels
  uint16_t margin_right = 0;  // inline right margin in pixels
  std::string href;           // hyperlink target (empty = no link)

  Run() = default;
  Run(const std::string& t, FontStyle s = FontStyle::Regular, bool br = false)
      : text(t), style(s), size_pct(100), breaking(br) {}
  Run(std::string&& t, FontStyle s = FontStyle::Regular, bool br = false)
      : text(std::move(t)), style(s), size_pct(100), breaking(br) {}
  Run(const char* t, FontStyle s = FontStyle::Regular, bool br = false)
      : text(t), style(s), size_pct(100), breaking(br) {}
  Run(const std::string& t, FontStyle s, int sz, bool br = false) : text(t), style(s), size_pct(sz), breaking(br) {}
  Run(std::string&& t, FontStyle s, int sz, bool br = false)
      : text(std::move(t)), style(s), size_pct(sz), breaking(br) {}
  Run(const char* t, FontStyle s, int sz, bool br = false) : text(t), style(s), size_pct(sz), breaking(br) {}

  bool operator==(const Run& o) const {
    return text == o.text && style == o.style && size_pct == o.size_pct && vertical_align == o.vertical_align &&
           breaking == o.breaking && margin_left == o.margin_left && margin_right == o.margin_right && href == o.href;
  }
};

// ---------------------------------------------------------------------------
// A text paragraph: a sequence of Runs
// ---------------------------------------------------------------------------

struct ImageRef {
  uint16_t key = 0;         // index into the EPUB's file entries / MRB image ref table
  uint16_t attr_width = 0;  // from HTML attributes or MRB image table; 0 = unknown
  uint16_t attr_height = 0;

  ImageRef() = default;
  ImageRef(uint16_t k, uint16_t w, uint16_t h) : key(k), attr_width(w), attr_height(h) {}
};

struct TextParagraph {
  std::vector<Run> runs;
  std::optional<Alignment> alignment;
  std::optional<int16_t> indent;
  std::optional<ImageRef> inline_image;  // small float image rendered at start of first line
  uint8_t line_height_pct = 100;         // line-height as % of default (100 = normal)
};

// ---------------------------------------------------------------------------
// Paragraph = Text | Image | Hr
// ---------------------------------------------------------------------------

enum class ParagraphType : uint8_t {
  Text,
  Image,
  Hr,
  PageBreak,
};

struct Paragraph {
  ParagraphType type = ParagraphType::Text;
  TextParagraph text;                      // valid when type == Text
  ImageRef image;                          // valid when type == Image
  std::optional<uint16_t> spacing_before;  // override para_spacing if set
  std::optional<uint8_t> hr_width_pct;     // HR width as percentage (0-100), nullopt = default (1/3)

  Paragraph() = default;

  static Paragraph make_text(TextParagraph&& t) {
    Paragraph p;
    p.type = ParagraphType::Text;
    p.text = std::move(t);
    return p;
  }
  static Paragraph make_image(uint16_t key, uint16_t w = 0, uint16_t h = 0) {
    Paragraph p;
    p.type = ParagraphType::Image;
    p.image = {key, w, h};
    return p;
  }
  static Paragraph make_hr() {
    Paragraph p;
    p.type = ParagraphType::Hr;
    return p;
  }
  static Paragraph make_page_break() {
    Paragraph p;
    p.type = ParagraphType::PageBreak;
    return p;
  }
};

// ---------------------------------------------------------------------------
// Chapter
// ---------------------------------------------------------------------------

struct Chapter {
  std::optional<std::string> title;
  std::vector<Paragraph> paragraphs;
};

// ---------------------------------------------------------------------------
// EPUB metadata
// ---------------------------------------------------------------------------

struct EpubMetadata {
  std::string title;
  std::optional<std::string> author;
  std::optional<std::string> language;
  std::optional<std::string> cover_id;
};

// ---------------------------------------------------------------------------
// EPUB spine item — references a file in the ZIP
// ---------------------------------------------------------------------------

struct SpineItem {
  uint16_t file_idx = 0;
};

// ---------------------------------------------------------------------------
// Table of contents entry
// ---------------------------------------------------------------------------
// Use a small StringPool to pack many small strings into one contiguous blob.
// This is reusable across other structures that need pooled strings.

struct TocEntry {
  StringRef label{};
  StringRef fragment{};  // empty => no fragment
  uint16_t file_idx = 0;
  uint8_t depth = 0;        // nesting depth in NCX (0 = top-level)
  uint16_t para_index = 0;  // paragraph index within chapter (stored in MRB v6+)
};

struct TableOfContents {
  std::vector<TocEntry> entries;
  StringPool pool;

  std::string_view label_view(const TocEntry& e) const { return e.label.view(pool); }
  std::string_view fragment_view(const TocEntry& e) const { return e.fragment.view(pool); }
  // Convenience overloads for legacy call sites that only have an index.
  std::string_view label_view(size_t idx) const { return entries[idx].label.view(pool); }

  // Add an entry during parsing; uses the pool to store strings.
  void add_entry(std::string_view label, uint16_t file_idx, uint8_t depth, std::string_view fragment = {},
                 uint16_t para_index = 0) {
    TocEntry te;
    te.label = pool.add(label);
    te.fragment = pool.add(fragment);
    te.file_idx = file_idx;
    te.depth = depth;
    te.para_index = para_index;
    entries.push_back(te);
  }
};

}  // namespace microreader
