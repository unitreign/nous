#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "ContentModel.h"

namespace microreader {

enum class TextTransform : uint8_t { None = 0, Uppercase, Lowercase, Capitalize };

// Configuration for resolving relative CSS units (em, %) to pixels.
struct CssConfig {
  uint16_t glyph_width = 12;     // pixels per em (font glyph width)
  uint16_t content_width = 440;  // content area width for % calculations
  uint16_t max_margin_pct = 15;  // max combined margin-left+right as % of content_width
};

// A CSS rule: the style properties we care about for e-book rendering.
struct CssRule {
  // Value fields
  int16_t indent = 0;
  uint8_t font_size_pct = 100;
  uint8_t line_height_pct = 100;
  uint16_t margin_left = 0;
  uint16_t margin_right = 0;
  uint16_t margin_top = 0;
  uint16_t margin_bottom = 0;
  uint8_t width_pct = 0;

  // Bitfield values
  Alignment alignment : 3;
  TextTransform text_transform : 3;
  VerticalAlign vertical_align : 3;
  bool italic : 1;
  bool bold : 1;
  bool is_float : 1;
  bool is_hidden : 1;
  bool page_break_before : 1;
  bool page_break_after : 1;
  bool list_style_none : 1;
  bool font_variant_small_caps : 1;
  bool border_top : 1;

  // Presence flags
  bool has_alignment_ : 1;
  bool has_italic_ : 1;
  bool has_bold_ : 1;
  bool has_indent_ : 1;
  bool has_font_size_pct_ : 1;
  bool has_margin_left_ : 1;
  bool has_margin_right_ : 1;
  bool has_margin_top_ : 1;
  bool has_margin_bottom_ : 1;
  bool has_is_float_ : 1;
  bool has_is_hidden_ : 1;
  bool has_page_break_before_ : 1;
  bool has_page_break_after_ : 1;
  bool has_text_transform_ : 1;
  bool has_vertical_align_ : 1;
  bool has_line_height_pct_ : 1;
  bool has_list_style_none_ : 1;
  bool has_font_variant_small_caps_ : 1;
  bool has_border_top_ : 1;
  bool has_width_pct_ : 1;

  CssRule() {
    alignment = Alignment::Start;
    text_transform = TextTransform::None;
    vertical_align = VerticalAlign::Baseline;
    italic = false;
    bold = false;
    is_float = false;
    is_hidden = false;
    page_break_before = false;
    page_break_after = false;
    list_style_none = false;
    font_variant_small_caps = false;
    border_top = false;

    has_alignment_ = false;
    has_italic_ = false;
    has_bold_ = false;
    has_indent_ = false;
    has_font_size_pct_ = false;
    has_margin_left_ = false;
    has_margin_right_ = false;
    has_margin_top_ = false;
    has_margin_bottom_ = false;
    has_is_float_ = false;
    has_is_hidden_ = false;
    has_page_break_before_ = false;
    has_page_break_after_ = false;
    has_text_transform_ = false;
    has_vertical_align_ = false;
    has_line_height_pct_ = false;
    has_list_style_none_ = false;
    has_font_variant_small_caps_ = false;
    has_border_top_ = false;
    has_width_pct_ = false;
  }

  // Provide minimal std::optional-like interface for getters
  struct OptAlignment {
    const CssRule& r;
    Alignment value_or(Alignment def) const {
      return r.has_alignment_ ? r.alignment : def;
    }
  };
  struct OptTextTransform {
    const CssRule& r;
    TextTransform value_or(TextTransform def) const {
      return r.has_text_transform_ ? r.text_transform : def;
    }
  };
  struct OptVerticalAlign {
    const CssRule& r;
    VerticalAlign value_or(VerticalAlign def) const {
      return r.has_vertical_align_ ? r.vertical_align : def;
    }
  };
  struct OptBool {
    bool has;
    bool val;
    bool value_or(bool def) const {
      return has ? val : def;
    }
  };
  struct OptInt16 {
    bool has;
    int16_t val;
    int16_t value_or(int16_t def) const {
      return has ? val : def;
    }
  };
  struct OptUint8 {
    bool has;
    uint8_t val;
    uint8_t value_or(uint8_t def) const {
      return has ? val : def;
    }
  };
  struct OptUint16 {
    bool has;
    uint16_t val;
    uint16_t value_or(uint16_t def) const {
      return has ? val : def;
    }
  };

  OptAlignment alignment_opt() const {
    return {*this};
  }
  OptTextTransform text_transform_opt() const {
    return {*this};
  }
  OptVerticalAlign vertical_align_opt() const {
    return {*this};
  }
  OptBool italic_opt() const {
    return {has_italic_, italic};
  }
  OptBool bold_opt() const {
    return {has_bold_, bold};
  }
  OptBool is_float_opt() const {
    return {has_is_float_, is_float};
  }
  OptBool is_hidden_opt() const {
    return {has_is_hidden_, is_hidden};
  }
  OptBool page_break_before_opt() const {
    return {has_page_break_before_, page_break_before};
  }
  OptBool page_break_after_opt() const {
    return {has_page_break_after_, page_break_after};
  }
  OptBool list_style_none_opt() const {
    return {has_list_style_none_, list_style_none};
  }
  OptBool font_variant_small_caps_opt() const {
    return {has_font_variant_small_caps_, font_variant_small_caps};
  }
  OptBool border_top_opt() const {
    return {has_border_top_, border_top};
  }

  OptInt16 indent_opt() const {
    return {has_indent_, indent};
  }
  OptUint8 font_size_pct_opt() const {
    return {has_font_size_pct_, font_size_pct};
  }
  OptUint8 line_height_pct_opt() const {
    return {has_line_height_pct_, line_height_pct};
  }
  OptUint16 margin_left_opt() const {
    return {has_margin_left_, margin_left};
  }
  OptUint16 margin_right_opt() const {
    return {has_margin_right_, margin_right};
  }
  OptUint16 margin_top_opt() const {
    return {has_margin_top_, margin_top};
  }
  OptUint16 margin_bottom_opt() const {
    return {has_margin_bottom_, margin_bottom};
  }
  OptUint8 width_pct_opt() const {
    return {has_width_pct_, width_pct};
  }

  // Setters
  void set_alignment(Alignment v) {
    alignment = v;
    has_alignment_ = true;
  }
  void set_text_transform(TextTransform v) {
    text_transform = v;
    has_text_transform_ = true;
  }
  void set_vertical_align(VerticalAlign v) {
    vertical_align = v;
    has_vertical_align_ = true;
  }
  void set_italic(bool v) {
    italic = v;
    has_italic_ = true;
  }
  void set_bold(bool v) {
    bold = v;
    has_bold_ = true;
  }
  void set_is_float(bool v) {
    is_float = v;
    has_is_float_ = true;
  }
  void set_is_hidden(bool v) {
    is_hidden = v;
    has_is_hidden_ = true;
  }
  void set_page_break_before(bool v) {
    page_break_before = v;
    has_page_break_before_ = true;
  }
  void set_page_break_after(bool v) {
    page_break_after = v;
    has_page_break_after_ = true;
  }
  void set_list_style_none(bool v) {
    list_style_none = v;
    has_list_style_none_ = true;
  }
  void set_font_variant_small_caps(bool v) {
    font_variant_small_caps = v;
    has_font_variant_small_caps_ = true;
  }
  void set_border_top(bool v) {
    border_top = v;
    has_border_top_ = true;
  }
  void set_width_pct(uint8_t v) {
    width_pct = v;
    has_width_pct_ = true;
  }

  void set_indent(int16_t v) {
    indent = v;
    has_indent_ = true;
  }
  void set_font_size_pct(uint8_t v) {
    font_size_pct = v;
    has_font_size_pct_ = true;
  }
  void set_line_height_pct(uint8_t v) {
    line_height_pct = v;
    has_line_height_pct_ = true;
  }
  void set_margin_left(uint16_t v) {
    margin_left = v;
    has_margin_left_ = true;
  }
  void set_margin_right(uint16_t v) {
    margin_right = v;
    has_margin_right_ = true;
  }
  void set_margin_top(uint16_t v) {
    margin_top = v;
    has_margin_top_ = true;
  }
  void set_margin_bottom(uint16_t v) {
    margin_bottom = v;
    has_margin_bottom_ = true;
  }

  bool has_any() const {
    return has_alignment_ || has_italic_ || has_bold_ || has_indent_ || has_font_size_pct_ || has_margin_left_ ||
           has_margin_right_ || has_margin_top_ || has_margin_bottom_ || has_is_float_ || has_is_hidden_ ||
           has_page_break_before_ || has_page_break_after_ || has_text_transform_ || has_vertical_align_ ||
           has_line_height_pct_ || has_list_style_none_ || has_font_variant_small_caps_ || has_border_top_ ||
           has_width_pct_;
  }

  // Parse declarations like "text-align: center; font-weight: bold"
  static CssRule parse(const char* declarations, size_t length, const CssConfig& config = CssConfig{});
  static CssRule parse(const std::string& s, const CssConfig& config = CssConfig{}) {
    return parse(s.c_str(), s.size(), config);
  }

  // Merge: rhs overrides lhs where present
  CssRule operator+(const CssRule& rhs) const;
};

// A simple CSS stylesheet. Matches selectors by element name, id, and class.
class CssStylesheet {
 public:
  CssStylesheet() = default;
  explicit CssStylesheet(const CssConfig& config) : config_(config) {}

  void set_config(const CssConfig& config) {
    config_ = config;
  }
  const CssConfig& config() const {
    return config_;
  }

  // Parse and add rules from a CSS string (contents of a <style> block or .css file).
  void extend_from_sheet(const char* css, size_t length);
  void extend_from_mut_sheet(char* css, size_t length);
  void extend_from_sheet(const std::string& s) {
    extend_from_sheet(s.c_str(), s.size());
  }

  // Look up cascaded style for an element.
  CssRule get(const char* element, const char* id, const char* cls) const;
  // Overload accepting lengths (avoids null-termination requirement).
  CssRule get(const char* element, size_t element_len, const char* id, size_t id_len, const char* cls,
              size_t cls_len) const;

  size_t rule_count() const {
    return rules_.size();
  }

  // Internal selector type (public for inline matching in .cpp)
  struct Selector {
    std::string element;
    std::string id;
    std::vector<std::string> classes;

    static bool try_parse(const char* s, size_t len, Selector& out);
    bool matches(const char* element, const char* id, const std::vector<std::string>& classes) const;
    uint32_t specificity() const;
  };

 private:
  CssConfig config_;
  std::deque<std::pair<Selector, CssRule>> rules_;

  static std::string filter_comments(const char* css, size_t length);
};

}  // namespace microreader
