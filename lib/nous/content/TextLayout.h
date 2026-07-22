#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ContentModel.h"
#include "Font.h"
#include "IParagraphSource.h"
#include "hyphenation/Hyphenation.h"

namespace microreader {

// ---------------------------------------------------------------------------
// Layout output types
// ---------------------------------------------------------------------------

struct LayoutWord {
  const char* text;  // pointer into original Run (stable for chapter lifetime)
  uint16_t len;      // byte length
  uint16_t x;        // pixel offset from left edge
  FontStyle style;
  uint8_t size_pct = 100;
  VerticalAlign vertical_align = VerticalAlign::Baseline;
  bool continues_prev = false;  // true if placed adjacent to previous word (no inter-word gap)
  const char* href = nullptr;   // hyperlink target (pointer into Run.href; nullptr = no link)
};

struct LayoutLine {
  std::vector<LayoutWord> words;
  bool hyphenated = false;   // true if line ends with a hyphen
  uint32_t text_offset = 0;  // byte offset of the first character from the start of the paragraph's text
};

// Non-owning view of a laid-out line. Words point into PageContent::word_pool.
// Valid for the lifetime of the owning PageContent object.
struct PageLine {
  struct Words {
    const LayoutWord* data_ = nullptr;
    uint16_t size_ = 0;
    const LayoutWord* begin() const {
      return data_;
    }
    const LayoutWord* end() const {
      return data_ + size_;
    }
    uint16_t size() const {
      return size_;
    }
    bool empty() const {
      return size_ == 0;
    }
    const LayoutWord& operator[](size_t i) const {
      return data_[i];
    }
  } words;
  bool hyphenated = false;
  uint32_t text_offset = 0;
};

// ---------------------------------------------------------------------------
// layout_paragraph() — break a single paragraph into lines
// ---------------------------------------------------------------------------

struct LayoutOptions {
  uint16_t width = 300;  // available line width in pixels
  std::optional<Alignment> align_override;
  uint16_t first_line_extra_indent = 0;  // extra left indent for inline images on first line
  HyphenationLang hyphenation_lang = HyphenationLang::None;
  bool override_publisher_fonts = false;

  LayoutOptions() = default;
  LayoutOptions(uint16_t w, std::optional<Alignment> a = std::nullopt) : width(w), align_override(a) {}
};

// ---------------------------------------------------------------------------
// Page layout — fit paragraphs onto a fixed-height page
// ---------------------------------------------------------------------------

struct PagePosition {
  uint16_t paragraph = 0;  // index into chapter.paragraphs
  uint16_t offset = 0;     // line index within a text paragraph; pixel row offset within an image paragraph (0 for hrs)
  uint32_t text_offset = 0;  // byte offset of the first character from the start of the paragraph's text

  PagePosition() = default;
  PagePosition(uint16_t p, uint16_t l, uint32_t to = 0) : paragraph(p), offset(l), text_offset(to) {}

  bool operator==(const PagePosition& o) const {
    return paragraph == o.paragraph && offset == o.offset && text_offset == o.text_offset;
  }
  bool operator!=(const PagePosition& o) const {
    return !(*this == o);
  }
  bool operator<(const PagePosition& o) const {
    if (paragraph != o.paragraph)
      return paragraph < o.paragraph;
    if (offset != o.offset)
      return offset < o.offset;
    return text_offset < o.text_offset;
  }
  bool operator<=(const PagePosition& o) const {
    return *this == o || *this < o;
  }
};

struct PageImageItem {
  uint16_t paragraph_index;
  uint16_t key;
  uint16_t width;
  uint16_t height;           // slice height (pixels rendered on this page)
  uint16_t x_offset;         // pixel offset from page left (for centering)
  uint16_t y_offset;         // pixel offset from page top (absolute, includes vertical centering)
  uint16_t y_crop = 0;       // rows into source image to skip (for partial/split images)
  uint16_t full_height = 0;  // full scaled image height (for decode: width × aspect)
};

struct PageHrItem {
  uint16_t x_offset;  // pixel offset from page left
  uint16_t y_offset;  // slot top (absolute, includes vertical centering); render adds font.y_advance()/2
  uint16_t width;     // line width
  uint16_t height;    // slot height (= font.y_advance())
};

struct PageTextItem {
  uint16_t paragraph_index;
  uint16_t line_index;
  PageLine line;          // non-owning; words point into PageContent::word_pool
  uint16_t y_offset;      // pixel offset from page top (absolute, includes vertical centering)
  uint16_t height = 0;    // line height in pixels
  uint16_t baseline = 0;  // baseline distance from top of line to baseline
  // Non-promoted inline image anchored to this line's baseline (line_index == 0 only).
  std::optional<PageImageItem> inline_image;
};

// A single content item on a page: text line, standalone image, or horizontal rule.
// Items are stored in top-to-bottom order in PageContent::items.
using PageContentItem = std::variant<PageTextItem, PageImageItem, PageHrItem>;

struct PageContent {
  std::vector<LayoutWord> word_pool;   // flat storage for all PageTextItem words (stable after construction)
  std::vector<PageContentItem> items;  // ordered top-to-bottom; y_offsets are absolute screen coords
  PagePosition start;
  PagePosition end;  // one-past-end position
  bool at_chapter_end = false;

  // Typed accessors — O(n) filter; prefer iterating items with std::visit in hot paths.
  std::vector<PageTextItem> text_items() const;
  std::vector<PageImageItem> image_items() const;
  std::vector<PageHrItem> hr_items() const;
};

struct PageOptions {
  uint16_t width = 300;
  uint16_t height = 400;
  uint16_t padding_top = 0;
  uint16_t padding_right = 10;
  uint16_t padding_bottom = 10;
  uint16_t padding_left = 10;
  uint16_t para_spacing = 8;                    // pixels between paragraphs (fallback when CSS spacing_before not set)
  uint16_t line_height_multiplier_percent = 0;  // 0 = use CSS/Book default. Otherwise scale by this percent.
  std::optional<Alignment> align_override;
  bool center_text = false;  // vertically center text content within the padded area
  bool override_publisher_fonts = false;

  PageOptions() = default;
  // Uniform padding constructor: sets all four sides to pad.
  PageOptions(uint16_t w, uint16_t h, uint16_t pad = 10, uint16_t ps = 8, std::optional<Alignment> a = std::nullopt)
      : width(w),
        height(h),
        padding_top(pad),
        padding_right(pad),
        padding_bottom(pad),
        padding_left(pad),
        para_spacing(ps),
        align_override(a) {}

  bool operator==(const PageOptions& o) const {
    return width == o.width && height == o.height && padding_top == o.padding_top && padding_right == o.padding_right &&
           padding_bottom == o.padding_bottom && padding_left == o.padding_left && para_spacing == o.para_spacing &&
           line_height_multiplier_percent == o.line_height_multiplier_percent && align_override == o.align_override &&
           center_text == o.center_text && override_publisher_fonts == o.override_publisher_fonts;
  }
};

// Optional callback for resolving image dimensions lazily at layout time.
// Called when attr_width or attr_height is 0. Returns true if size was resolved.
using ImageSizeQuery = std::function<bool(uint16_t key, uint16_t& w, uint16_t& h)>;

// Minimum image slice height when splitting an image across pages.
// The actual threshold is min(kMinImageSliceH, img_h/4) so small images stay proportional.
static constexpr uint16_t kMinImageSliceH = 64;

// ---------------------------------------------------------------------------
// TextLayout — stateful layout engine
//
// Stores font, options, paragraph source, image size query, and current
// page position. Call layout() / layout_backward() without arguments.
// Position must be updated manually after navigation (set_position).
// ---------------------------------------------------------------------------

class TextLayout {
 public:
  TextLayout() = default;
  explicit TextLayout(IFont& font) : font_(&font) {}
  TextLayout(IFont& font, const PageOptions& opts) : font_(&font), opts_(opts) {}
  TextLayout(IFont& font, const PageOptions& opts, IParagraphSource& source, ImageSizeQuery size_fn = {})
      : font_(&font), opts_(opts), source_(&source), size_fn_(std::move(size_fn)) {}
  TextLayout(IFont& font, const PageOptions& opts, IParagraphSource& source, PagePosition start,
             ImageSizeQuery size_fn = {})
      : font_(&font), opts_(opts), source_(&source), size_fn_(std::move(size_fn)), position_(start) {}

  PagePosition position() const {
    return position_;
  }
  void set_position(PagePosition pos) {
    position_ = pos;
  }

  // Replace the paragraph source (e.g. after loading a new chapter).
  void set_source(IParagraphSource& source) {
    source_ = &source;
    cache_valid_ = false;
  }

  // Replace the font (e.g. after a proportional font becomes available).
  void set_font(IFont& font) {
    if (font_ == &font)
      return;
    font_ = &font;
    cache_valid_ = false;
  }

  // Replace options (e.g. after a screen resize).
  void set_options(const PageOptions& opts) {
    if (opts_ == opts)
      return;
    opts_ = opts;
    cache_valid_ = false;
  }

  // Replace the image size query (e.g. when a book is opened).
  void set_image_size_fn(ImageSizeQuery fn) {
    size_fn_ = std::move(fn);
    cache_valid_ = false;
  }

  // Set the hyphenation language (detected from EPUB metadata).
  void set_hyphenation_lang(HyphenationLang lang) {
    if (hyphenation_lang_ == lang)
      return;
    hyphenation_lang_ = lang;
    cache_valid_ = false;
  }

  // Invalidate cached line breaks when the metrics of the current font object
  // change in place (for example, BitmapFontSet selecting a new base size).
  // The next layout rebuilds only the paragraphs needed for the current page.
  void invalidate_cache() {
    cache_valid_ = false;
  }

  // Break a single paragraph into lines using the stored font.
  std::vector<LayoutLine> layout_paragraph(const LayoutOptions& opts, const TextParagraph& para) const;

  // Layout the page starting at position(). Does not change position().
  PageContent layout() const;

  // Layout the page that ends at position() (backward fill). Does not change
  // position(). Use the returned page.start to navigate to the previous page.
  PageContent layout_backward() const;

  // Returns true if pos is mid-way through a promoted inline image (i.e. the
  // offset is a pixel row inside the promoted-image region of a Text paragraph).
  // Used by navigation to snap to the start of the paragraph so the full image
  // is shown on the next page.
  bool is_mid_promoted_image(PagePosition pos) const;

  // Returns the offset (in the paragraph's idx space) just past the promoted
  // inline image of paragraph `para`, i.e. the offset where the first text
  // line after the image begins. Returns 0 if the paragraph has no promoted
  // image. Used by backward navigation to land on a page that contains the
  // FULL image rather than a slice.
  uint16_t promoted_image_end_offset(uint16_t para) const;

  // If `pos` is mid-image (standalone Image paragraph or promoted inline image),
  // returns a position snapped to the bottom of that image so that
  // layout_backward() will produce a page containing the full image.
  // Returns `pos` unchanged if it is not mid-image.
  PagePosition snap_to_image_end(PagePosition pos) const;

  // Uses the layout cache (with the currently configured font/options)
  // to sync `offset` (line index) based on the absolute `text_offset` of the page position.
  PagePosition resolve_stable_position(PagePosition pos) const;

  // Implementation-internal types. Declared public so that .cpp-scope helper
  // functions (which are not class members) can reference them without
  // triggering MSVC C2248 private-access errors.
  struct PageItem {
    enum Kind { TextLine, Image, Hr, Empty, PageBreak, Spacer } kind;
    uint16_t para_idx, line_idx;
    const LayoutLine* line_ptr = nullptr;  // TextLine only: points into LaidOutParagraph cache
    uint16_t height, baseline = 0;
    uint16_t img_key = 0, img_w = 0, img_h = 0, img_x = 0, img_y_crop = 0;
  };
  struct CollectResult {
    std::vector<PageItem> items;
    PagePosition boundary;
    bool at_chapter_end = false;
  };

  struct InlineImageInfo {
    uint16_t key = 0, width = 0, height = 0;
    bool promoted = false, has_image = false;
  };

  // Laid-out paragraph: result of the expensive per-paragraph work (line-breaking,
  // image scaling, inline image resolution). Stored in a small ring-buffer cache
  // so that a paragraph spanning two pages is not re-laid-out on every layout() call.
  struct LaidOutParagraph {
    uint16_t para_idx = UINT16_MAX;  // UINT16_MAX = empty slot
    ParagraphType type = ParagraphType::Text;

    // ParagraphType::Text:
    bool text_runs_empty = false;  // true when para.text.runs is empty
    std::vector<LayoutLine> lines;
    InlineImageInfo inline_img;
    std::vector<uint16_t> line_heights;                       // pre-computed per-line heights
    std::vector<uint16_t> line_baselines;                     // pre-computed per-line baselines
    uint16_t inline_extra = 0;                                // extra px on line 0 for non-promoted inline image
    uint16_t promoted_w = 0, promoted_h = 0, promoted_x = 0;  // scaled promoted inline image

    // ParagraphType::Image (standalone), Hr, empty Text:
    uint16_t block_height = 0;                              // Image: scaled h (0=unknown); Hr/empty-text: font advance
    uint16_t img_key = 0, img_w = 0, img_h = 0, img_x = 0;  // Image paragraph only
    uint16_t min_slice_h = 0;     // Image: minimum forward-slice height; smaller slices are deferred
    uint16_t min_cut_h = 0;       // Image: minimum cut-off when slicing; ensure cut is visually obvious
    uint16_t leading_spacer = 0;  // spacing_before baked into index space: idx 0 = Spacer, real content starts at 1

    bool empty() const {
      return para_idx == UINT16_MAX;
    }

    // Result of both collect() and collect_backward().
    // next_idx is the cursor position to pass on the next call to the same function:
    //   collect(r.next_idx, ...)        — advance forward
    //   collect_backward(r.next_idx, ...) — advance backward
    // For text lines next_idx advances by 1. For image slices it advances by the slice height in pixels.
    struct Collected {
      PageItem item;
      size_t next_idx = 0;
    };

    // Returns the item starting at collect-index `idx` that fits within `available` pixels,
    // or nullopt when the paragraph is exhausted or the item doesn't fit.
    std::optional<Collected> collect(size_t idx, uint16_t available) const;

    // Symmetric backward operation: given end_idx (exclusive upper bound, i.e. the next_idx
    // of the item we want) and available pixels, return the item that ends there.
    std::optional<Collected> collect_backward(size_t end_idx, uint16_t available, bool require_full_height = false) const;
  };

 private:
  CollectResult collect_page_items(PagePosition pos) const;
  CollectResult collect_page_items_backward(PagePosition end_pos) const;
  PageContent assemble_page(std::vector<PageItem>& items, PagePosition start, PagePosition end,
                            bool at_chapter_end) const;
  static InlineImageInfo resolve_inline_image(const TextParagraph& text_para, uint16_t content_width,
                                              const ImageSizeQuery& sp);
  const LaidOutParagraph& get_laid_out_(size_t pi) const;

  IFont* font_ = nullptr;
  PageOptions opts_;
  IParagraphSource* source_ = nullptr;
  ImageSizeQuery size_fn_;
  PagePosition position_;
  HyphenationLang hyphenation_lang_ = HyphenationLang::None;

  static constexpr size_t kCacheCapacity = 16;
  mutable std::array<LaidOutParagraph, kCacheCapacity> para_cache_{};
  mutable size_t cache_next_ = 0;
  mutable bool cache_valid_ = false;
};

}  // namespace microreader

#ifdef ESP_PLATFORM
// Per-page layout sub-timings (microseconds). Reset by TextLayout::layout(); read after it returns.
extern int64_t g_layout_hyph_us;     // time spent in find_hyphen_break()
extern int64_t g_layout_metrics_us;  // time spent in font.word_width()
extern int64_t g_layout_para_us;     // total time in layout_para_lines() calls (incl. hyph+metrics)
extern int g_layout_cache_misses;    // paragraphs that triggered a fresh layout_para_lines()
#endif
