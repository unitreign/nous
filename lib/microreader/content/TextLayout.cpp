#include "TextLayout.h"

#include <algorithm>
#include <cstdio>
#include <optional>

#include "hyphenation/Hyphenation.h"

// Set to 1 to print a step-by-step trace of forward + backward page collection.
// Output goes to stdout; safe to enable in desktop builds while debugging.
#ifndef MR_LAYOUT_TRACE
#define MR_LAYOUT_TRACE 0
#endif

#if MR_LAYOUT_TRACE
#define MR_TRACE(...)         \
  do {                        \
    std::printf("[layout] "); \
    std::printf(__VA_ARGS__); \
    std::printf("\n");        \
  } while (0)
#else
#define MR_TRACE(...) ((void)0)
#endif

namespace microreader {

#if MR_LAYOUT_TRACE
static const char* trace_kind(int k) {
  switch (k) {
    case TextLayout::PageItem::TextLine:
      return "Text";
    case TextLayout::PageItem::Image:
      return "Image";
    case TextLayout::PageItem::Hr:
      return "Hr";
    case TextLayout::PageItem::PageBreak:
      return "PgBrk";
    case TextLayout::PageItem::Empty:
      return "Empty";
    case TextLayout::PageItem::Spacer:
      return "Spacer";
  }
  return "?";
}
#endif

// ---------------------------------------------------------------------------
// PageContent typed accessors
// ---------------------------------------------------------------------------

std::vector<PageTextItem> PageContent::text_items() const {
  std::vector<PageTextItem> r;
  for (const auto& ci : items)
    if (const PageTextItem* p = std::get_if<PageTextItem>(&ci))
      r.push_back(*p);
  return r;
}

std::vector<PageImageItem> PageContent::image_items() const {
  std::vector<PageImageItem> r;
  for (const auto& ci : items)
    if (const PageImageItem* p = std::get_if<PageImageItem>(&ci))
      r.push_back(*p);
  return r;
}

std::vector<PageHrItem> PageContent::hr_items() const {
  std::vector<PageHrItem> r;
  for (const auto& ci : items)
    if (const PageHrItem* p = std::get_if<PageHrItem>(&ci))
      r.push_back(*p);
  return r;
}

static bool is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Returns the byte length of the UTF-8 sequence starting at text[i].
static size_t utf8_char_len(const char* text, size_t len, size_t i) {
  uint8_t b = static_cast<uint8_t>(text[i]);
  if (b < 0x80)
    return 1;
  if (b < 0xC0)
    return 1;  // stray continuation byte
  if (b < 0xE0)
    return (i + 1 < len) ? 2 : 1;
  if (b < 0xF0)
    return (i + 2 < len) ? 3 : 1;
  return (i + 3 < len) ? 4 : 1;
}

// Returns true if the UTF-8 character at text[i] is a CJK character that can
// break freely at every character boundary (hiragana, katakana, CJK ideographs,
// CJK symbols/punctuation, fullwidth forms).
static bool is_cjk_break_char(const char* text, size_t len, size_t i) {
  uint8_t b0 = static_cast<uint8_t>(text[i]);
  if (b0 < 0xE0 || i + 2 >= len)
    return false;
  uint8_t b1 = static_cast<uint8_t>(text[i + 1]);
  uint8_t b2 = static_cast<uint8_t>(text[i + 2]);
  uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
  // U+3000–U+9FFF: CJK symbols/punct, kana, CJK unified ideographs
  // U+F900–U+FAFF: CJK compatibility ideographs
  // U+FF00–U+FFEF: Halfwidth/fullwidth forms
  return (cp >= 0x3000 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

static size_t ws_len(const char* text, size_t len, size_t i) {
  uint8_t b = static_cast<uint8_t>(text[i]);
  if (b <= 0x7F)
    return is_ws(text[i]) ? 1 : 0;
  if (b == 0xC2 && i + 1 < len && static_cast<uint8_t>(text[i + 1]) == 0xA0)
    return 2;
  if (b == 0xE2 && i + 2 < len) {
    uint8_t b1 = static_cast<uint8_t>(text[i + 1]), b2 = static_cast<uint8_t>(text[i + 2]);
    if (b1 == 0x80 && ((b2 >= 0x80 && b2 <= 0x8A) || b2 == 0xA8 || b2 == 0xA9 || b2 == 0xAF))
      return 3;
    if (b1 == 0x81 && b2 == 0x9F)
      return 3;
  }
  if (b == 0xE3 && i + 2 < len && static_cast<uint8_t>(text[i + 1]) == 0x80 &&
      static_cast<uint8_t>(text[i + 2]) == 0x80)
    return 3;
  return 0;
}

struct WordSpan {
  size_t start, len;
  bool no_space_before;  // true when this span is adjacent (no whitespace) to the previous span
};

static std::vector<WordSpan> split_words(const char* text, size_t text_len) {
  std::vector<WordSpan> spans;
  size_t i = 0;
  while (i < text_len) {
    // Skip whitespace.
    size_t wl;
    bool had_space = false;
    while (i < text_len && (wl = ws_len(text, text_len, i)) > 0) {
      i += wl;
      had_space = true;
    }
    if (i >= text_len)
      break;
    // CJK character: each character is its own break opportunity with no
    // inter-character space inserted.
    if (is_cjk_break_char(text, text_len, i)) {
      size_t clen = utf8_char_len(text, text_len, i);
      bool no_spc = !had_space && !spans.empty();
      spans.push_back({i, clen, no_spc});
      i += clen;
      continue;
    }
    // Latin/other word: consume until whitespace or a CJK break char.
    size_t start = i;
    while (i < text_len && ws_len(text, text_len, i) == 0 && !is_cjk_break_char(text, text_len, i))
      ++i;
    // Suppress space if this non-CJK word immediately followed a CJK char
    // (e.g. "東京Tokyo" — no gap between CJK and Latin).
    bool no_spc = !had_space && !spans.empty();
    spans.push_back({start, i - start, no_spc});
  }
  return spans;
}

// ---------------------------------------------------------------------------
// Alignment helpers
// ---------------------------------------------------------------------------

static void justify_words(uint16_t room, uint16_t line_width, uint16_t space_width, std::vector<LayoutWord>& words) {
  if (words.size() <= 1)
    return;
  size_t gaps = 0;
  for (size_t i = 1; i < words.size(); ++i)
    if (!words[i].continues_prev)
      ++gaps;
  if (gaps == 0)
    return;
  // If the per-gap spread would exceed line_width/8, fall back to a fixed
  // 2× space-width gap so lines look consistent rather than over-stretched.
  const uint16_t natural_gap = room / static_cast<uint16_t>(gaps);
  const uint16_t max_gap = line_width > 0 ? line_width / 8 : (space_width * 2);
  if (natural_gap > max_gap) {
    // Fixed spacing: each gap gets exactly space_width*2 (or less if room is tight).
    const uint16_t fixed = space_width * 2;
    room = static_cast<uint16_t>(
        std::min(static_cast<uint32_t>(fixed) * static_cast<uint32_t>(gaps), static_cast<uint32_t>(room)));
  }
  if (room == 0)
    return;
  uint16_t spc = room / static_cast<uint16_t>(gaps);
  uint16_t rem = room % static_cast<uint16_t>(gaps);
  uint16_t off = 0;
  for (size_t i = 1; i < words.size(); ++i) {
    if (!words[i].continues_prev) {
      off += spc + (rem > 0 ? 1 : 0);
      if (rem > 0)
        --rem;
    }
    words[i].x += off;
  }
}

static void align_line(Alignment alignment, uint16_t room, uint16_t line_width, uint16_t space_width,
                       std::vector<LayoutWord>& words, bool is_last) {
  if (words.empty())
    return;
  switch (alignment) {
    case Alignment::Center:
      for (auto& w : words)
        w.x += room / 2;
      break;
    case Alignment::End:
      for (auto& w : words)
        w.x += room;
      break;
    case Alignment::Justify:
      if (!is_last) {
        justify_words(room, line_width, space_width, words);
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// layout_paragraph() — greedy line-breaking with styled runs
// ---------------------------------------------------------------------------

static std::vector<LayoutLine> layout_para_lines(const IFont& font, const LayoutOptions& opts,
                                                 const TextParagraph& para,
                                                 HyphenationLang hyph_lang = HyphenationLang::None) {
  const uint16_t max_width = opts.width;
  const uint16_t space_width = font.char_width(' ', FontStyle::Regular);

  Alignment align = para.alignment.value_or(Alignment::Start);
  if (opts.align_override.has_value()) {
    // Structural alignments (Center/End) from the book should not be overridden by normal body text settings
    // (Left/Justify)
    if (opts.align_override.value() == Alignment::Start || opts.align_override.value() == Alignment::Justify) {
      if (align != Alignment::Center && align != Alignment::End) {
        align = opts.align_override.value();
      }
    } else {
      align = opts.align_override.value();  // Force Center or Right on everything if the user explicitly chose that
    }
  }

  const int16_t indent = para.indent.value_or(0);

  std::vector<LayoutLine> lines;
  LayoutLine current;
  uint16_t x = opts.first_line_extra_indent + ((indent > 0) ? static_cast<uint16_t>(indent) : 0);
  uint16_t cur_line_width = max_width;
  bool first_line = true, prev_run_ended_space = true;
  uint32_t para_byte_offset = 0;

  // For Center/End alignment, words start at margin_left (not 0), which would
  // otherwise skew the centering. This helper normalises word x to [0..text_width]
  // before computing room so that align_line produces a symmetric result.
  auto flush_line = [&](uint16_t lw, std::vector<LayoutWord>& words, bool is_last) {
    if (!words.empty() && (align == Alignment::Center || align == Alignment::End)) {
      uint16_t x_first = words.front().x;
      uint16_t text_w = x > x_first ? x - x_first : 0;
      for (auto& w : words)
        w.x -= x_first;
      uint16_t room = lw > text_w ? lw - text_w : 0;
      align_line(align, room, lw, space_width, words, is_last);
    } else {
      uint16_t room = lw > x ? lw - x : 0;
      align_line(align, room, lw, space_width, words, is_last);
    }
  };

  for (const auto& run : para.runs) {
    uint8_t eff_size_pct = opts.override_publisher_fonts ? 100 : run.size_pct;
    const char* text = run.text.c_str();
    const size_t text_len = run.text.size();
    const char* run_href = run.href.empty() ? nullptr : run.href.c_str();
    const uint16_t line_width = (run.margin_right < max_width) ? (max_width - run.margin_right) : max_width;
    cur_line_width = line_width;

    if (current.words.empty() && run.margin_left > 0) {
      if (first_line && indent < 0) {
        int16_t combined = static_cast<int16_t>(run.margin_left) + indent;
        x = (combined > 0) ? static_cast<uint16_t>(combined) : 0;
      } else {
        x = run.margin_left;
      }
    }

    if (current.words.empty()) {
      size_t i = 0, wl;
      while (i < text_len && (wl = ws_len(text, text_len, i)) > 0) {
        x += font.char_width(' ', run.style, eff_size_pct);
        i += wl;
      }
    }

    auto spans = split_words(text, text_len);
    bool first_word_of_run = true;
    for (const auto& span : spans) {
      const char* word_ptr = text + span.start;
      uint16_t word_len = span.len;
      bool needs_space = !current.words.empty();
      if (first_word_of_run && !prev_run_ended_space && text_len > 0 && ws_len(text, text_len, 0) == 0)
        needs_space = false;
      if (span.no_space_before)
        needs_space = false;
      first_word_of_run = false;

      // Loop handles multi-line hyphenation: each iteration places a chunk or
      // emits a hyphenated prefix and continues with the remaining suffix.
      while (true) {
        uint16_t word_w = font.word_width(word_ptr, word_len, run.style, eff_size_pct);
        uint16_t needed = word_w + (needs_space ? space_width : 0);

        if (current.words.empty()) {
          current.text_offset = para_byte_offset + static_cast<uint32_t>(word_ptr - text);
        }

        if (x + needed <= line_width) {
          // Fits normally.
          if (needs_space)
            x += space_width;
          current.words.push_back(LayoutWord{word_ptr, word_len, x, run.style, eff_size_pct, run.vertical_align,
                                             !needs_space && !current.words.empty()});
          current.words.back().href = run_href;
          x += word_w;
          break;
        }
        // Doesn't fit — try to hyphenate at the best break point.
        uint16_t avail =
            line_width > x + (needs_space ? space_width : 0) ? line_width - x - (needs_space ? space_width : 0) : 0;
        bool prefix_has_hyphen = false;
        size_t split =
            find_hyphen_break(font, word_ptr, word_len, run.style, eff_size_pct, hyph_lang, avail, prefix_has_hyphen);
        if (split > 0) {
          // Emit prefix + hyphen, flush, then loop with the suffix.
          const uint16_t prefix_w = font.word_width(word_ptr, static_cast<uint16_t>(split), run.style, eff_size_pct);
          // Don't add a synthetic hyphen if the prefix already ends with one.
          const uint16_t hyphen_w = prefix_has_hyphen ? 0 : font.char_width('-', run.style, eff_size_pct);
          if (needs_space)
            x += space_width;
          current.words.push_back(LayoutWord{word_ptr, static_cast<uint16_t>(split), x, run.style, eff_size_pct,
                                             run.vertical_align, false});
          current.words.back().href = run_href;
          x += prefix_w;
          if (!prefix_has_hyphen) {
            current.words.push_back(LayoutWord{"-", 1, x, run.style, eff_size_pct, run.vertical_align, true});
            current.words.back().href = run_href;
            x += hyphen_w;
          }
          current.hyphenated = true;
          flush_line(line_width, current.words, false);
          lines.push_back(std::move(current));
          current = LayoutLine{};
          x = run.margin_left;
          first_line = false;
          word_ptr += split;
          word_len -= static_cast<uint16_t>(split);
          needs_space = false;
          continue;
        }
        if (current.words.empty()) {
          // No hyphenation and line is empty — force placement to avoid infinite loop.
          current.words.push_back(
              LayoutWord{word_ptr, word_len, x, run.style, eff_size_pct, run.vertical_align, false});
          current.words.back().href = run_href;
          x += word_w;
          break;
        }
        // No hyphenation possible — flush line and retry on the next line.
        flush_line(line_width, current.words, false);
        lines.push_back(std::move(current));
        current = LayoutLine{};
        x = run.margin_left;
        first_line = false;
        needs_space = false;
      }
    }

    prev_run_ended_space = text_len == 0;
    if (text_len > 0) {
      if (is_ws(text[text_len - 1]))
        prev_run_ended_space = true;
      else if (text_len >= 2 && ws_len(text, text_len, text_len - 2) == 2)
        prev_run_ended_space = true;
      else if (text_len >= 3 && ws_len(text, text_len, text_len - 3) == 3)
        prev_run_ended_space = true;
    }

    if (run.breaking) {
      flush_line(cur_line_width, current.words, true);
      lines.push_back(std::move(current));
      current = LayoutLine{};
      x = 0;
      first_line = false;
      prev_run_ended_space = true;
    }

    para_byte_offset += static_cast<uint32_t>(text_len);
  }

  if (!current.words.empty()) {
    flush_line(cur_line_width, current.words, true);
    lines.push_back(std::move(current));
  }
  return lines;
}

// ---------------------------------------------------------------------------
// Page layout types and shared helpers
// ---------------------------------------------------------------------------

static void scale_image(const PageOptions& opts, uint16_t content_width, uint16_t& img_w, uint16_t& img_h,
                        uint16_t& x_off) {
  const uint16_t fw = opts.width;
  if (img_w >= fw / 2) {
    if (img_w != fw) {
      img_h = static_cast<uint16_t>(static_cast<uint32_t>(img_h) * fw / img_w);
      img_w = fw;
    }
  } else {
    if (img_w > content_width) {
      img_h = static_cast<uint16_t>(static_cast<uint32_t>(img_h) * content_width / img_w);
      img_w = content_width;
    }
  }
  if (img_w == 0)
    img_w = 1;
  if (img_h == 0)
    img_h = 1;
  if (img_w >= fw)
    x_off = 0;
  else if (img_w > content_width)
    x_off = fw > img_w ? (fw - img_w) / 2 : 0;
  else
    x_off = opts.padding_left + (content_width > img_w ? (content_width - img_w) / 2 : 0);
}

static uint16_t compute_line_height(const IFont& font, const LayoutLine& line, uint16_t pct) {
  uint16_t h = 0;
  for (const auto& w : line.words) {
    uint16_t wh = font.y_advance(w.size_pct);
    if (wh > h)
      h = wh;
  }
  if (h == 0)
    h = font.y_advance();
  if (pct != 100) {
    h = static_cast<uint16_t>(h * pct / 100);
    if (h < 1)
      h = 1;
  }
  return h;
}

static uint16_t line_baseline(const IFont& font, const LayoutLine& line) {
  uint16_t bl = 0;
  for (const auto& w : line.words) {
    uint16_t b = font.baseline(w.size_pct);
    if (b > bl)
      bl = b;
  }
  if (bl == 0)
    bl = font.baseline();
  return bl;
}

TextLayout::InlineImageInfo TextLayout::resolve_inline_image(const TextParagraph& text_para, uint16_t content_width,
                                                             const ImageSizeQuery& sp) {
  InlineImageInfo info;
  if (!text_para.inline_image.has_value())
    return info;
  const auto& img = *text_para.inline_image;
  info.has_image = true;
  info.key = img.key;
  if (img.attr_width > 0 && img.attr_height > 0) {
    info.width = img.attr_width;
    info.height = img.attr_height;
  } else if (sp)
    sp(img.key, info.width, info.height);
  if (info.width > 0 && info.height > 0)
    info.promoted = (info.width > content_width / 3 || info.height > 120);
  return info;
}

// get_laid_out_() — lay out one paragraph and cache the result.
// The ring-buffer cache (capacity kCacheCapacity) is invalidated whenever the
// font, options, paragraph source, or image-size callback changes.
const TextLayout::LaidOutParagraph& TextLayout::get_laid_out_(size_t pi) const {
  if (!cache_valid_) {
    for (auto& e : para_cache_)
      e = LaidOutParagraph{};
    cache_next_ = 0;
    cache_valid_ = true;
  }
  for (auto& e : para_cache_)
    if (!e.empty() && e.para_idx == static_cast<uint16_t>(pi))
      return e;

  LaidOutParagraph& slot = para_cache_[cache_next_];
  cache_next_ = (cache_next_ + 1) % kCacheCapacity;

  const IFont& font = *font_;
  const PageOptions& opts = opts_;
  const ImageSizeQuery& sp = size_fn_;
  const uint16_t cw = opts.width - opts.padding_left - opts.padding_right;
  const auto& para = source_->paragraph(pi);

  slot = LaidOutParagraph{};
  slot.para_idx = static_cast<uint16_t>(pi);
  slot.type = para.type;
  if (pi == 0 && para.spacing_before.has_value() && *para.spacing_before > 0)
    slot.leading_spacer = *para.spacing_before;

  switch (para.type) {
    case ParagraphType::Text: {
      if (para.text.runs.empty()) {
        slot.text_runs_empty = true;
        slot.block_height = font.y_advance();
        break;
      }
      InlineImageInfo img = resolve_inline_image(para.text, cw, sp);
      slot.inline_img = img;
      LayoutOptions lo;
      lo.width = cw;
      lo.align_override = opts.align_override;
      lo.override_publisher_fonts = opts.override_publisher_fonts;
      if (img.has_image && img.width > 0 && img.height > 0 && !img.promoted)
        lo.first_line_extra_indent = img.width + 4;
      slot.lines = layout_para_lines(font, lo, para.text, hyphenation_lang_);
      slot.line_heights.resize(slot.lines.size());
      slot.line_baselines.resize(slot.lines.size());
      for (size_t i = 0; i < slot.lines.size(); ++i) {
        uint16_t pct =
            opts.line_height_multiplier_percent != 0 ? opts.line_height_multiplier_percent : para.text.line_height_pct;
        slot.line_heights[i] = compute_line_height(font, slot.lines[i], pct);
        slot.line_baselines[i] = line_baseline(font, slot.lines[i]);
      }
      if (!img.promoted && img.has_image && img.width > 0 && img.height > 0 && !slot.lines.empty()) {
        uint16_t bl0 = slot.line_baselines[0];
        if (img.height > bl0)
          slot.inline_extra = img.height - bl0;
      }
      if (img.promoted && img.width > 0 && img.height > 0) {
        slot.promoted_w = img.width;
        slot.promoted_h = img.height;
        scale_image(opts, cw, slot.promoted_w, slot.promoted_h, slot.promoted_x);
        // Clamp promoted image height to page height so it fits without slicing.
        const uint16_t ph = opts.height > opts.padding_top + opts.padding_bottom
                                ? opts.height - opts.padding_top - opts.padding_bottom
                                : opts.height;
        if (slot.promoted_h > ph && slot.promoted_h > 0) {
          slot.promoted_w = static_cast<uint16_t>(static_cast<uint32_t>(slot.promoted_w) * ph / slot.promoted_h);
          if (slot.promoted_w == 0)
            slot.promoted_w = 1;
          slot.promoted_h = ph;
          slot.promoted_x = slot.promoted_w < opts.width ? (opts.width - slot.promoted_w) / 2 : 0;
        }
        // Minimum slice/cut thresholds for the promoted image region.
        // At least kMinImageSliceH, or 25% of the image height if that is smaller.
        slot.min_slice_h = std::min(kMinImageSliceH, static_cast<uint16_t>(slot.promoted_h / 4));
        if (slot.min_slice_h == 0)
          slot.min_slice_h = 1;
        slot.min_cut_h = slot.min_slice_h;
      }
      break;
    }
    case ParagraphType::Image: {
      uint16_t w = para.image.attr_width, h = para.image.attr_height;
      if ((w == 0 || h == 0) && sp)
        sp(para.image.key, w, h);
      slot.img_key = para.image.key;
      if (w > 0 && h > 0) {
        uint16_t x;
        scale_image(opts, cw, w, h, x);
        // Clamp to page height so the image always fits on one page without slicing.
        const uint16_t ph = opts.height > opts.padding_top + opts.padding_bottom
                                ? opts.height - opts.padding_top - opts.padding_bottom
                                : opts.height;
        if (h > ph && h > 0) {
          w = static_cast<uint16_t>(static_cast<uint32_t>(w) * ph / h);
          if (w == 0)
            w = 1;
          h = ph;
          x = w < opts.width ? (opts.width - w) / 2 : 0;
        }
        slot.img_w = w;
        slot.img_h = h;
        slot.img_x = x;
        slot.block_height = h;

        // Minimum slice height: at least kMinImageSliceH, or 25% of image height if smaller.
        slot.min_slice_h = std::min(kMinImageSliceH, static_cast<uint16_t>(h / 4));
        if (slot.min_slice_h == 0)
          slot.min_slice_h = 1;
        slot.min_cut_h = slot.min_slice_h;
      }
      break;
    }
    case ParagraphType::Hr:
      slot.block_height = font.y_advance();
      break;
    case ParagraphType::PageBreak:
      break;
  }
  return slot;
}

// ---------------------------------------------------------------------------
// assemble_page helpers
// ---------------------------------------------------------------------------

// Bake a pixel offset into all item y_offsets (and attached inline images).
static void bake_y(PageContent& page, uint16_t v_off) {
  if (v_off == 0)
    return;
  for (auto& ci : page.items) {
    std::visit([v_off](auto& it) { it.y_offset += v_off; }, ci);
    if (PageTextItem* ti = std::get_if<PageTextItem>(&ci))
      if (ti->inline_image.has_value())
        ti->inline_image->y_offset += v_off;
  }
}

// Build page.items from raw PageItems, applying paragraph spacing.
// Returns the total y consumed (before any padding/centering).
template <typename GetLP>
static uint16_t build_page_items(PageContent& page, std::vector<TextLayout::PageItem>& raw, const PageOptions& opts,
                                 const IFont& font, IParagraphSource& source, bool is_chapter_start, GetLP get_lp) {
  const uint16_t dy = font.y_advance();
  const uint16_t cw = opts.width - opts.padding_left - opts.padding_right;
  uint16_t y = 0, prev_para = UINT16_MAX;

  for (auto& item : raw) {
    if (prev_para != UINT16_MAX && item.para_idx != prev_para) {
      uint16_t spc = source.paragraph(item.para_idx).spacing_before.value_or(opts.para_spacing);
      y += spc;
    }

    switch (item.kind) {
      case TextLayout::PageItem::TextLine: {
        PageTextItem ti{item.para_idx, item.line_idx, std::move(item.layout_line), y, item.height,
                        item.baseline, std::nullopt};
        if (item.line_idx == 0) {
          const auto& lp = get_lp(item.para_idx);
          if (!lp.inline_img.promoted && lp.inline_img.has_image && lp.inline_img.width > 0 &&
              lp.inline_img.height > 0) {
            uint16_t bl_y = y + item.baseline;
            uint16_t img_y = bl_y >= lp.inline_img.height ? bl_y - lp.inline_img.height : 0;
            ti.inline_image = PageImageItem{item.para_idx,        lp.inline_img.key, lp.inline_img.width,
                                            lp.inline_img.height, opts.padding_left, img_y};
          }
        }
        page.items.push_back(std::move(ti));
        break;
      }
      case TextLayout::PageItem::Image:
        page.items.push_back(PageImageItem{item.para_idx, item.img_key, item.img_w, item.height, item.img_x, y,
                                           item.img_y_crop, item.img_h});
        break;
      case TextLayout::PageItem::Hr:
        page.items.push_back(PageHrItem{opts.padding_left, y, cw, dy});  // y_offset = slot top; render adds dy/2
        break;
      case TextLayout::PageItem::Empty:
      case TextLayout::PageItem::PageBreak:
      case TextLayout::PageItem::Spacer:
        break;
    }
    y += item.height;
    if (item.kind != TextLayout::PageItem::Spacer)
      prev_para = item.para_idx;
  }
  return y;
}

struct PageFlags {
  bool has_text, has_standalone_img, has_hr;
};
static PageFlags classify_items(const PageContent& page) {
  PageFlags f{};
  for (const auto& ci : page.items) {
    if (std::holds_alternative<PageTextItem>(ci))
      f.has_text = true;
    else if (std::holds_alternative<PageImageItem>(ci))
      f.has_standalone_img = true;
    else if (std::holds_alternative<PageHrItem>(ci))
      f.has_hr = true;
  }
  return f;
}

// Uniform helpers — all item types store y_offset as slot top.
static uint16_t& item_y(PageContentItem& ci) {
  return std::visit([](auto& it) -> uint16_t& { return it.y_offset; }, ci);
}
static uint16_t item_height(const PageContentItem& ci) {
  return std::visit([](const auto& it) { return it.height; }, ci);
}
static uint16_t item_para_idx(const PageContentItem& ci) {
  return std::visit(
      [](const auto& it) -> uint16_t {
        if constexpr (std::is_same_v<std::decay_t<decltype(it)>, PageHrItem>)
          return 0;
        else
          return it.paragraph_index;
      },
      ci);
}
static bool item_is_block(const PageContentItem& ci) {
  return !std::holds_alternative<PageTextItem>(ci);
}

// Distribute slack proportionally across inter-item gaps when the page is nearly full.
// padding_bottom defines the distance from the screen bottom to the last text line's baseline,
// so slack is measured as (ph - last_text_baseline). After spreading the last baseline sits
// exactly at ph, and bake_y(padding_top) moves it to height - padding_bottom.
static void spread_text_items(PageContent& page, const PageOptions& opts, const IFont& font, uint16_t y) {
  const uint16_t dy = font.y_advance();
  const uint16_t ph =
      opts.height > opts.padding_top + opts.padding_bottom ? opts.height - opts.padding_top - opts.padding_bottom : 0;

  // Collect spreadable item indices (skip non-promoted inline images).
  std::vector<size_t> idxs;
  for (size_t i = 0; i < page.items.size(); ++i) {
    const PageImageItem* img = std::get_if<PageImageItem>(&page.items[i]);
    if (img && img->full_height == 0)
      continue;  // non-promoted inline — skip
    idxs.push_back(i);
  }
  if (idxs.size() < 2)
    return;

  const size_t N = idxs.size();

  // Compute slack so that the LAST spreadable item lands at the page bottom.
  //   * If the last item is a text line, anchor by its baseline (so padding_bottom
  //     measures from screen bottom to the last text baseline).
  //   * Otherwise (HR / image), anchor by its slot bottom — bottom-aligning the
  //     block to ph. Anchoring by an earlier text baseline would let the spread
  //     push trailing blocks (e.g. a chapter-end HR) below the page boundary.
  int32_t slack;
  {
    size_t last = idxs.back();
    if (const PageTextItem* ti = std::get_if<PageTextItem>(&page.items[last])) {
      uint16_t last_baseline_y = item_y(page.items[last]) + ti->baseline;
      slack = static_cast<int32_t>(ph) - static_cast<int32_t>(last_baseline_y);
    } else {
      uint16_t last_bottom = item_y(page.items[last]) + item_height(page.items[last]);
      slack = static_cast<int32_t>(ph) - static_cast<int32_t>(last_bottom);
    }
  }

  if (slack <= 0 || slack >= static_cast<int32_t>(dy) * 2)
    return;

  // Compute raw gaps and weights between consecutive spreadable items.
  std::vector<int32_t> raw_gaps(N - 1), weights(N - 1);
  int32_t total_weight = 0;
  for (size_t i = 0; i < N - 1; ++i) {
    auto& a = page.items[idxs[i]];
    auto& b = page.items[idxs[i + 1]];
    raw_gaps[i] = std::max(INT32_C(0), static_cast<int32_t>(item_y(b)) - static_cast<int32_t>(item_y(a)) -
                                           static_cast<int32_t>(item_height(a)));
    bool inter_para = !item_is_block(a) && !item_is_block(b) && item_para_idx(a) != item_para_idx(b);
    weights[i] = (item_is_block(a) || item_is_block(b)) ? 8 : (inter_para ? 4 : 1);
    total_weight += weights[i];
  }
  if (total_weight == 0)
    return;

  // Distribute slack: base pixels per weight unit, remainder spread left-to-right.
  std::vector<int32_t> extra(N - 1, 0);
  int32_t base = slack / total_weight, leftover = slack % total_weight, units = 0;
  for (size_t i = 0; i < N - 1; ++i)
    for (int32_t w = 0; w < weights[i]; ++w)
      extra[i] += base + (units++ >= total_weight - leftover ? 1 : 0);

  // Apply new y positions directly onto items — no intermediate Slot list.
  int32_t y_acc = item_y(page.items[idxs[0]]);
  for (size_t i = 0; i < N; ++i) {
    item_y(page.items[idxs[i]]) = static_cast<uint16_t>(y_acc);
    y_acc += static_cast<int32_t>(item_height(page.items[idxs[i]]));
    if (i < N - 1)
      y_acc += raw_gaps[i] + extra[i];
  }

  // Re-anchor inline images to their (now-moved) text item's baseline.
  for (auto& ci : page.items) {
    if (PageTextItem* ti = std::get_if<PageTextItem>(&ci); ti && ti->inline_image.has_value()) {
      auto& img = *ti->inline_image;
      uint16_t bl_y = ti->y_offset + ti->baseline;
      img.y_offset = bl_y >= img.height ? bl_y - img.height : 0;
    }
  }
}

// ---------------------------------------------------------------------------
// assemble_page() — convert ordered PageItems into PageContent.
// Applies paragraph spacing, image-only centering, and line spreading.
// y_offsets in the returned PageContent are absolute screen coordinates
// (padding_top already baked in — no separate vertical_offset field).
// ---------------------------------------------------------------------------

PageContent TextLayout::assemble_page(std::vector<PageItem>& items, PagePosition start, PagePosition end,
                                      bool at_chapter_end) const {
  PageContent page;
  page.start = start;
  page.end = end;
  page.at_chapter_end = at_chapter_end;

  bool is_cs = (start.paragraph == 0 && start.offset == 0);
  uint16_t y = build_page_items(page, items, opts_, *font_, *source_, is_cs,
                                [this](size_t pi) -> const LaidOutParagraph& { return get_laid_out_(pi); });

  auto [has_text, has_img, has_hr] = classify_items(page);

  if (!has_text && has_img && !has_hr) {
    bake_y(page, static_cast<uint16_t>(opts_.height > y ? (opts_.height - y) / 2 : 0));
    return page;
  }

  if (opts_.center_text && has_text)
    spread_text_items(page, opts_, *font_, y);

  bake_y(page, opts_.padding_top);
  return page;
}

// ---------------------------------------------------------------------------
// LaidOutParagraph::collect — static helpers, one per paragraph type
// ---------------------------------------------------------------------------

using LaidOut = TextLayout::LaidOutParagraph;
using Collected = TextLayout::LaidOutParagraph::Collected;
using PageItem = TextLayout::PageItem;

// Direction-unified text collect.
//
// forward=true:  idx is the start of the desired item (Collected::idx semantics).
// forward=false: idx is the exclusive end of the desired item (Collected::next_idx semantics).
//
// For the promoted-image pixel region the index space is pixel offsets [0..promoted_h).
// Backward automatically clamps `available` to produce the exact slice that ends at idx,
// so that round-tripping forward→backward and backward→forward is always consistent.
static std::optional<Collected> collect_text(const LaidOut& lp, size_t idx, uint16_t available, bool forward) {
  // --- Step 1: translate idx to start_idx, clamping available if needed ---

  size_t start_idx;
  if (forward) {
    start_idx = idx;
  } else {
    // idx is end_idx (exclusive upper bound).
    if (lp.text_runs_empty) {
      if (idx != 1)
        return std::nullopt;
      start_idx = 0;
    } else if (lp.inline_img.promoted && lp.promoted_h > 0 && idx <= (size_t)lp.promoted_h) {
      // Pixel region: clamp available so the forward builder produces exactly [start..idx).
      uint16_t slice_h = static_cast<uint16_t>(std::min((size_t)available, idx));
      start_idx = idx - slice_h;
      available = slice_h;
    } else {
      start_idx = idx - 1;
    }
  }

  // --- Step 2: build item from start_idx ---

  if (lp.text_runs_empty) {
    if (start_idx != 0)
      return std::nullopt;
    // Empty items always fit — they contribute no height to the page.
    return Collected{
        PageItem{PageItem::Empty, lp.para_idx, 0, {}, lp.block_height},
        forward ? size_t(1) : size_t(0)
    };
  }

  // Promoted inline image pixel region.
  if (lp.inline_img.promoted && lp.promoted_h > 0 && start_idx < (size_t)lp.promoted_h) {
    uint16_t remaining = lp.promoted_h - static_cast<uint16_t>(start_idx);
    uint16_t slice_h = std::min(remaining, available);
    if (slice_h == 0)
      return std::nullopt;
    // Enforce minimum slice and minimum cut-off thresholds (both directions).
    if (slice_h < remaining) {
      if (lp.min_cut_h > 0 && (remaining - slice_h) < lp.min_cut_h) {
        uint16_t adjusted = remaining > lp.min_cut_h ? static_cast<uint16_t>(remaining - lp.min_cut_h) : 0;
        slice_h = adjusted;
        if (!forward)
          start_idx =
              static_cast<size_t>(static_cast<uint16_t>(start_idx) + (std::min(remaining, available) - slice_h));
      }
      if (lp.min_slice_h > 0 && slice_h < lp.min_slice_h)
        return std::nullopt;
      if (slice_h == 0)
        return std::nullopt;
    }
    PageItem item{PageItem::Image,
                  lp.para_idx,
                  0,
                  {},
                  slice_h,
                  0,
                  lp.inline_img.key,
                  lp.promoted_w,
                  lp.promoted_h,
                  lp.promoted_x,
                  static_cast<uint16_t>(start_idx)};
    return Collected{std::move(item), forward ? start_idx + slice_h : start_idx};
  }

  // Text line (plain paragraph, or lines after the promoted-image region).
  size_t line_idx = start_idx;
  if (lp.inline_img.promoted && lp.promoted_h > 0)
    line_idx = start_idx - lp.promoted_h;

  if (line_idx >= lp.lines.size())
    return std::nullopt;

  uint16_t lh = lp.line_heights[line_idx];
  uint16_t bl = lp.line_baselines[line_idx];
  if (line_idx == 0 && lp.inline_extra > 0) {
    lh += lp.inline_extra;
    bl += lp.inline_extra;
  }
  // Accept a line if its baseline fits — spread_text_items aligns by baseline so
  // a line whose descenders extend slightly past the slot boundary is OK.
  if (bl > available)
    return std::nullopt;

  PageItem item{PageItem::TextLine, lp.para_idx, static_cast<uint16_t>(line_idx), lp.lines[line_idx], lh, bl};
  return Collected{std::move(item), forward ? start_idx + 1 : start_idx};
}

static std::optional<Collected> collect_image(const LaidOut& lp, size_t idx, uint16_t available, bool forward) {
  if (lp.block_height == 0 || available == 0)
    return std::nullopt;
  size_t start_idx;
  if (forward) {
    if (idx >= (size_t)lp.block_height)
      return std::nullopt;
    start_idx = idx;
  } else {
    if (idx == 0 || idx > (size_t)lp.block_height)
      return std::nullopt;
    uint16_t slice_h = static_cast<uint16_t>(std::min((size_t)available, idx));
    start_idx = idx - slice_h;
    available = slice_h;
  }
  uint16_t remaining = lp.block_height - static_cast<uint16_t>(start_idx);
  uint16_t slice_h = std::min(remaining, available);
  if (slice_h == 0)
    return std::nullopt;
  // Enforce minimum slice and minimum cut-off thresholds (both directions).
  if (slice_h < remaining) {
    // If cut-off would be too small, reduce slice_h so the cut is obvious.
    if (lp.min_cut_h > 0 && (remaining - slice_h) < lp.min_cut_h) {
      uint16_t adjusted = remaining > lp.min_cut_h ? static_cast<uint16_t>(remaining - lp.min_cut_h) : 0;
      slice_h = adjusted;
    }
    // If the resulting slice is too small, defer entirely.
    if (lp.min_slice_h > 0 && slice_h < lp.min_slice_h)
      return std::nullopt;
    if (slice_h == 0)
      return std::nullopt;
    // Recompute start_idx after adjustment (backward path).
    if (!forward)
      start_idx = static_cast<uint16_t>(idx) - slice_h;
  }
  PageItem item{PageItem::Image,
                lp.para_idx,
                0,
                {},
                slice_h,
                0,
                lp.img_key,
                lp.img_w,
                lp.img_h,
                lp.img_x,
                static_cast<uint16_t>(start_idx)};
  return Collected{std::move(item), forward ? start_idx + slice_h : start_idx};
}

static std::optional<Collected> collect_hr(const LaidOut& lp, size_t idx, uint16_t available, bool forward) {
  if (forward ? idx != 0 : idx != 1)
    return std::nullopt;
  if (lp.block_height > available)
    return std::nullopt;
  return Collected{
      PageItem{PageItem::Hr, lp.para_idx, 0, {}, lp.block_height},
      forward ? size_t(1) : size_t(0)
  };
}

static std::optional<Collected> collect_page_break(const LaidOut& lp, size_t idx, uint16_t /*available*/,
                                                   bool forward) {
  if (forward ? idx != 0 : idx != 1)
    return std::nullopt;
  return Collected{
      PageItem{PageItem::PageBreak, lp.para_idx, 0, {}, 0},
      forward ? size_t(1) : size_t(0)
  };
}

std::optional<Collected> TextLayout::LaidOutParagraph::collect(size_t idx, uint16_t available) const {
  // Index 0 is the leading Spacer when spacing_before is set; real content starts at 1.
  if (leading_spacer > 0) {
    if (idx == 0) {
      if (leading_spacer > available)
        return std::nullopt;
      return Collected{
          PageItem{PageItem::Spacer, para_idx, 0, {}, leading_spacer},
          size_t(1)
      };
    }
    idx -= 1;  // shift to internal index space
  }
  std::optional<Collected> r;
  switch (type) {
    case ParagraphType::Text:
      r = collect_text(*this, idx, available, true);
      break;
    case ParagraphType::Image:
      r = collect_image(*this, idx, available, true);
      break;
    case ParagraphType::Hr:
      r = collect_hr(*this, idx, available, true);
      break;
    case ParagraphType::PageBreak:
      r = collect_page_break(*this, idx, available, true);
      break;
  }
  if (r && leading_spacer > 0)
    r->next_idx += 1;  // shift next_idx back to external index space
  return r;
}

std::optional<Collected> TextLayout::LaidOutParagraph::collect_backward(size_t end_idx, uint16_t available) const {
  if (end_idx == 0 || available == 0)
    return std::nullopt;
  // end_idx==1 means "item ending at index 1, starting at 0" which is the Spacer.
  if (leading_spacer > 0 && end_idx == 1) {
    if (leading_spacer > available)
      return std::nullopt;
    return Collected{
        PageItem{PageItem::Spacer, para_idx, 0, {}, leading_spacer},
        size_t(0)
    };
  }
  size_t inner_end = leading_spacer > 0 ? end_idx - 1 : end_idx;
  std::optional<Collected> r;
  switch (type) {
    case ParagraphType::Text:
      r = collect_text(*this, inner_end, available, false);
      break;
    case ParagraphType::Image:
      r = collect_image(*this, inner_end, available, false);
      break;
    case ParagraphType::Hr:
      r = collect_hr(*this, inner_end, available, false);
      break;
    case ParagraphType::PageBreak:
      r = collect_page_break(*this, inner_end, available, false);
      break;
  }
  // Shift next_idx back: inner 0 → external 1 (Spacer slot), inner N → external N+1.
  if (r && leading_spacer > 0)
    r->next_idx += 1;
  return r;
}

// ---------------------------------------------------------------------------
// collect_para_items — collect items from one paragraph into the page.
// Returns break_idx: the idx at which collection stopped.
// ---------------------------------------------------------------------------

static size_t collect_para_items(const LaidOut& lp, size_t start_idx, uint16_t spc, uint16_t ph, uint16_t& used,
                                 bool& has_content, bool& page_full, bool& pending_page_break, PagePosition& boundary,
                                 std::vector<PageItem>& items) {
  MR_TRACE("  fwd para %u: start_idx=%zu spc=%u used=%u ph=%u", (unsigned)lp.para_idx, start_idx, (unsigned)spc,
           (unsigned)used, (unsigned)ph);
  size_t break_idx = start_idx;
  for (size_t idx = start_idx; !page_full; ++idx) {
    break_idx = idx;
    uint16_t gap = (idx == start_idx && !items.empty()) ? spc : 0;
    uint16_t avail = items.empty() ? ph : (used + gap < ph ? ph - used - gap : 0);

    if (avail == 0) {
      auto probe = lp.collect(idx, ph);
      if (probe) {
        if (probe->item.kind == PageItem::PageBreak) {
          boundary = {static_cast<uint16_t>(lp.para_idx + 1), 0, 0};
          pending_page_break = has_content;
        } else {
          uint32_t to = (lp.type == ParagraphType::Text && idx < lp.lines.size()) ? lp.lines[idx].text_offset : 0;
          boundary = {lp.para_idx, static_cast<uint16_t>(idx), to};
          page_full = true;
          MR_TRACE("    fwd avail=0 -> page_full at {p=%u,off=%zu}", (unsigned)lp.para_idx, idx);
        }
      }
      break;
    }

    auto r = lp.collect(idx, avail);
    if (!r) {
      MR_TRACE("    fwd idx=%zu avail=%u -> NO FIT (exhausted or too tall)", idx, (unsigned)avail);
      break;
    }

    if (r->item.kind == PageItem::PageBreak) {
      boundary = {static_cast<uint16_t>(lp.para_idx + 1), 0};
      pending_page_break = has_content;
      MR_TRACE("    fwd PageBreak -> boundary={p=%u,0}", (unsigned)(lp.para_idx + 1));
      break;
    }

    MR_TRACE("    fwd idx=%zu kind=%s h=%u bl=%u avail=%u gap=%u -> used %u->%u", idx, trace_kind(r->item.kind),
             (unsigned)r->item.height, (unsigned)r->item.baseline, (unsigned)avail, (unsigned)gap, (unsigned)used,
             (unsigned)(used + gap + r->item.height));

    used += gap + r->item.height;
    has_content |= (r->item.kind != PageItem::Empty);
    size_t next_idx = r->next_idx;
    items.push_back(std::move(r->item));
    idx = next_idx - 1;  // loop does ++idx
  }
  return break_idx;
}

// ---------------------------------------------------------------------------
// collect_page_items — fill a page by iterating paragraphs.
// ---------------------------------------------------------------------------

TextLayout::CollectResult TextLayout::collect_page_items(PagePosition pos) const {
  const uint16_t ph = opts_.height - opts_.padding_top - opts_.padding_bottom;
  const size_t pcnt = source_->paragraph_count();

  std::vector<PageItem> items;
  uint16_t used = 0;
  bool has_content = false, page_full = false, pending_page_break = false;
  PagePosition boundary = pos;

  for (size_t pi = (size_t)pos.paragraph; !page_full && pi < pcnt; ++pi) {
    const LaidOutParagraph& lp = get_laid_out_(pi);

    if (pending_page_break && lp.type != ParagraphType::PageBreak) {
      page_full = true;
      break;
    }

    const auto& para_spc = source_->paragraph(pi).spacing_before;
    // spc=0 for the starting paragraph (its Spacer, if any, is part of its own collect index space).
    int spc_i = (pi == (size_t)pos.paragraph) ? 0 : static_cast<int>(para_spc.value_or(opts_.para_spacing));
    uint16_t spc = static_cast<uint16_t>(spc_i < 0 ? 0 : spc_i);
    size_t start_idx = (pi == (size_t)pos.paragraph) ? (size_t)pos.offset : 0;

    size_t break_idx =
        collect_para_items(lp, start_idx, spc, ph, used, has_content, page_full, pending_page_break, boundary, items);

    if (!page_full && !items.empty()) {
      auto probe = lp.collect(break_idx, ph);
      if (probe && probe->item.kind != PageItem::PageBreak) {
        uint32_t to =
            (lp.type == ParagraphType::Text && break_idx < lp.lines.size()) ? lp.lines[break_idx].text_offset : 0;
        boundary = {static_cast<uint16_t>(pi), static_cast<uint16_t>(break_idx), to};
        page_full = true;
      }
    }

    if (!page_full)
      boundary = {static_cast<uint16_t>(pi + 1), 0, 0};
  }

  return {std::move(items), boundary, !page_full};
}

// ---------------------------------------------------------------------------
// TextLayout methods
// ---------------------------------------------------------------------------

std::vector<LayoutLine> TextLayout::layout_paragraph(const LayoutOptions& opts, const TextParagraph& para) const {
  return layout_para_lines(*font_, opts, para, opts.hyphenation_lang);
}

PageContent TextLayout::layout() const {
  MR_TRACE("=== layout() FORWARD from {p=%u,off=%u,to=%u} ===", (unsigned)position_.paragraph,
           (unsigned)position_.offset, (unsigned)position_.text_offset);
  auto c = collect_page_items(position_);
  MR_TRACE("=== forward done: items=%zu boundary={p=%u,off=%u} at_end=%d ===", c.items.size(),
           (unsigned)c.boundary.paragraph, (unsigned)c.boundary.offset, (int)c.at_chapter_end);
  return assemble_page(c.items, position_, c.boundary, c.at_chapter_end);
}

// ---------------------------------------------------------------------------
// collect_page_items_backward helpers
// ---------------------------------------------------------------------------

// Returns the exclusive-end collect-index for a fully exhausted paragraph.
static size_t paragraph_end_idx(const LaidOut& lp) {
  size_t base;
  switch (lp.type) {
    case ParagraphType::Text:
      if (lp.text_runs_empty)
        base = 1;
      else if (lp.inline_img.promoted && lp.promoted_h > 0)
        base = (size_t)lp.promoted_h + lp.lines.size();
      else
        base = lp.lines.size();
      break;
    case ParagraphType::Image:
      base = lp.block_height;  // 0 when image size is unknown
      break;
    case ParagraphType::Hr:
    case ParagraphType::PageBreak:
      base = 1;
      break;
    default:
      base = 0;
  }
  // Account for the leading Spacer occupying external index 0.
  return base + (lp.leading_spacer > 0 ? 1 : 0);
}

// Mirror of collect_para_items, going backward.
// Collects from end_idx down to 0 (or until the page is full).
// desc is the font's descender height (y_advance - baseline).  When a leading
// spacer at index 0 doesn't quite fit (overflow ≤ desc), it is accepted anyway
// because the overflow was caused by a baseline-fit text line earlier on the
// page.  This is safe: mathematical analysis shows the spacer only fails to fit
// in backward when used_fwd > ph (baseline-fit overflow), never for normal pages.
// Returns the remaining start offset within the paragraph (0 = fully consumed).
static size_t collect_para_items_bwd(const LaidOut& lp, size_t end_idx, uint16_t spc, uint16_t ph, uint16_t desc,
                                     uint16_t& used, bool& page_full, std::vector<PageItem>& rev_items) {
  MR_TRACE("  bwd para %u: end_idx=%zu spc=%u used=%u ph=%u desc=%u", (unsigned)lp.para_idx, end_idx, (unsigned)spc,
           (unsigned)used, (unsigned)ph, (unsigned)desc);
  size_t idx = end_idx;
  bool first_item = true;
  while (idx > 0 && !page_full) {
    uint16_t gap = (first_item && !rev_items.empty()) ? spc : 0;
    uint16_t avail = (used + gap < ph) ? static_cast<uint16_t>(ph - used - gap) : 0;

    // Leading spacer at index 0 of a paragraph: extend available by the font's
    // descender height to absorb baseline-fit overflow from the last text line.
    bool is_spacer = (idx == 1 && lp.leading_spacer > 0 && end_idx > 1);
    uint16_t effective_avail = is_spacer ? static_cast<uint16_t>(avail + desc) : avail;

    if (effective_avail == 0) {
      auto probe = lp.collect_backward(idx, ph);
      if (probe) {
        page_full = true;
        MR_TRACE("    bwd idx=%zu avail=0 -> page_full", idx);
      }
      break;
    }

    auto r = lp.collect_backward(idx, effective_avail);
    if (!r) {
      // Distinguish exhausted vs. item too tall for remaining space.
      auto probe = lp.collect_backward(idx, ph);
      if (probe) {
        page_full = true;
        MR_TRACE("    bwd idx=%zu avail=%u -> item too tall, page_full", idx, (unsigned)effective_avail);
      } else {
        MR_TRACE("    bwd idx=%zu -> exhausted", idx);
      }
      break;
    }

    // Symmetric baseline-fit rule:
    //   * The FIRST backward-collected item is the LAST item in forward order.
    //     If it's a text line, forward layout allows its descender to extend
    //     past the page bottom, so backward must count only its baseline toward
    //     `used` — otherwise the budget for items above it is too tight and a
    //     line that forward would have placed at the top would be rejected
    //     here (e.g. BaselineFit_ForwardBackwardRoundTrip).
    //   * EVERY OTHER backward-collected text item must fit by its FULL line
    //     height. Forward only tolerates descender overflow on its last item;
    //     allowing it mid-page in backward would yield a page whose start, when
    //     re-rendered forward, drops the bottom-most item (e.g. an HR sitting
    //     after a tall paragraph at chapter end — see HrBackwardTest).
    const bool is_first_on_page = rev_items.empty();
    if (!is_first_on_page && r->item.kind == PageItem::TextLine && r->item.height > effective_avail) {
      page_full = true;
      MR_TRACE("    bwd idx=%zu kind=Text h=%u > avail=%u (non-first) -> REJECT, page_full", idx,
               (unsigned)r->item.height, (unsigned)effective_avail);
      break;
    }
    uint16_t consumed = r->item.height;
    if (is_first_on_page && r->item.kind == PageItem::TextLine)
      consumed = r->item.baseline;

    MR_TRACE("    bwd idx=%zu kind=%s h=%u bl=%u avail=%u gap=%u first=%d consumed=%u -> used %u->%u next_idx=%zu", idx,
             trace_kind(r->item.kind), (unsigned)r->item.height, (unsigned)r->item.baseline, (unsigned)effective_avail,
             (unsigned)gap, (int)is_first_on_page, (unsigned)consumed, (unsigned)used,
             (unsigned)(used + gap + consumed), r->next_idx);

    used += gap + consumed;
    first_item = false;
    idx = r->next_idx;
    rev_items.push_back(std::move(r->item));
  }
  return idx;
}

// ---------------------------------------------------------------------------
// collect_page_items_backward — fill a page by iterating paragraphs backward.
// ---------------------------------------------------------------------------

TextLayout::CollectResult TextLayout::collect_page_items_backward(PagePosition end_pos) const {
  const uint16_t ph = opts_.height - opts_.padding_top - opts_.padding_bottom;
  // Extend backward budget by the normal-font descender height so that a
  // leading spacer at para 0 is accepted even when a baseline-fit text line
  // caused a tiny overflow (≤ desc pixels) in the forward pass.
  const uint16_t desc = static_cast<uint16_t>(font_->y_advance() - font_->baseline());
  const size_t pcnt = source_->paragraph_count();

  if (pcnt == 0 || (end_pos.paragraph == 0 && end_pos.offset == 0))
    return {
        {},
        {0, 0, 0},
        false
    };

  std::vector<PageItem> rev_items;
  uint16_t used = 0;
  bool page_full = false;
  bool stopped_at_page_break = false;
  PagePosition start = {0, 0, 0};

  // Clamp paragraph index to valid range.
  const size_t end_pi = std::min((size_t)end_pos.paragraph, pcnt);

  // First paragraph to process:
  //   offset > 0 and within range → partial collection within that paragraph.
  //   otherwise                   → full collection of the previous paragraph.
  size_t pi;
  if ((size_t)end_pos.paragraph < pcnt && end_pos.offset > 0)
    pi = (size_t)end_pos.paragraph;
  else {
    if (end_pi == 0)
      return {
          {},
          {0, 0, 0},
          false
      };
    pi = end_pi - 1;
  }

  while (!page_full && !stopped_at_page_break) {
    const LaidOutParagraph& lp = get_laid_out_(pi);

    if (lp.type == ParagraphType::PageBreak) {
      if (!rev_items.empty()) {
        // Content before a PageBreak belongs to the previous logical page.
        // Stop here; the start of this page is right after the break.
        start = {static_cast<uint16_t>(pi + 1), 0, 0};
        stopped_at_page_break = true;
      }
      // If rev_items is empty, skip the PageBreak (mirrors forward behaviour:
      // a page-break at the start of a page is ignored, not stop immediately).
      if (pi == 0)
        break;
      --pi;
      continue;
    }

    // How far into this paragraph do we collect?
    size_t para_end_idx;
    if (pi == (size_t)end_pos.paragraph && end_pos.offset > 0 && (size_t)end_pos.paragraph < pcnt)
      para_end_idx = end_pos.offset;
    else
      para_end_idx = paragraph_end_idx(lp);

    // Inter-paragraph spacing: gap between pi and the next paragraph (pi+1).
    // spacing_before[pi+1] is applied by assemble_page when pi+1's first item follows pi's.
    // Only budget it once items from later paragraphs are already collected.
    int spc_i = (!rev_items.empty())
                    ? static_cast<int>(source_->paragraph(pi + 1).spacing_before.value_or(opts_.para_spacing))
                    : 0;
    uint16_t spc = static_cast<uint16_t>(spc_i < 0 ? 0 : spc_i);

    size_t remaining = collect_para_items_bwd(lp, para_end_idx, spc, ph, desc, used, page_full, rev_items);

    // Update start only when at least one item was collected from this paragraph.
    if (!rev_items.empty() && remaining < para_end_idx) {
      uint32_t to =
          (lp.type == ParagraphType::Text && remaining < lp.lines.size()) ? lp.lines[remaining].text_offset : 0;
      start = {static_cast<uint16_t>(pi), static_cast<uint16_t>(remaining), to};
    }

    if (pi == 0)
      break;
    --pi;
  }

  std::reverse(rev_items.begin(), rev_items.end());

  bool at_chapter_end = ((size_t)end_pos.paragraph >= pcnt);
  // boundary holds the start of the backward page (what assemble_page needs as `start`)
  return {std::move(rev_items), start, at_chapter_end};
}

PageContent TextLayout::layout_backward() const {
  MR_TRACE("=== layout_backward() from {p=%u,off=%u,to=%u} ===", (unsigned)position_.paragraph,
           (unsigned)position_.offset, (unsigned)position_.text_offset);
  auto c = collect_page_items_backward(position_);
  MR_TRACE("=== backward done: items=%zu start={p=%u,off=%u} at_end=%d ===", c.items.size(),
           (unsigned)c.boundary.paragraph, (unsigned)c.boundary.offset, (int)c.at_chapter_end);
  return assemble_page(c.items, c.boundary, position_, c.at_chapter_end);
}

bool TextLayout::is_mid_promoted_image(PagePosition pos) const {
  if (pos.offset == 0 || !source_)
    return false;
  const size_t pcnt = source_->paragraph_count();
  if ((size_t)pos.paragraph >= pcnt)
    return false;
  const auto& para = source_->paragraph(pos.paragraph);
  if (para.type != ParagraphType::Text)
    return false;
  const LaidOutParagraph& lp = get_laid_out_(pos.paragraph);
  return lp.inline_img.promoted && lp.promoted_h > 0 && (size_t)pos.offset < (size_t)lp.promoted_h;
}

uint16_t TextLayout::promoted_image_end_offset(uint16_t para) const {
  if (!source_)
    return 0;
  const size_t pcnt = source_->paragraph_count();
  if ((size_t)para >= pcnt)
    return 0;
  const auto& p = source_->paragraph(para);
  if (p.type != ParagraphType::Text)
    return 0;
  const LaidOutParagraph& lp = get_laid_out_(para);
  if (!lp.inline_img.promoted || lp.promoted_h == 0)
    return 0;
  return lp.promoted_h;
}

PagePosition TextLayout::resolve_stable_position(PagePosition pos) const {
  if (!source_)
    return pos;

  const size_t pcnt = source_->paragraph_count();
  if ((size_t)pos.paragraph >= pcnt)
    return pos;

  const LaidOutParagraph& lp = get_laid_out_((size_t)pos.paragraph);
  // Only adjust text paragraphs
  if (lp.type != ParagraphType::Text || lp.lines.empty())
    return pos;

  // If we are mid-promoted-image, the pixel offset is stable enough.
  if (lp.inline_img.promoted && lp.promoted_h > 0 && pos.offset < lp.promoted_h)
    return pos;

  // Find the last line that starts at or before the recorded offset.
  uint16_t found_idx = 0;
  for (uint16_t i = 0; i < lp.lines.size(); ++i) {
    if (lp.lines[i].text_offset <= pos.text_offset) {
      found_idx = i;
    } else {
      break;
    }
  }

  uint16_t new_offset = found_idx;
  if (lp.inline_img.promoted && lp.promoted_h > 0)
    new_offset += lp.promoted_h;

  return {pos.paragraph, new_offset, lp.lines[found_idx].text_offset};
}

}  // namespace microreader
