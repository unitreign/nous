#include "CssParser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace microreader {

// ---------------------------------------------------------------------------
// CSS length parsing helper: px, em, %, pt, rem
// ---------------------------------------------------------------------------

// Parse a CSS length value (already lowercased) to pixels.
// For em/rem, uses glyph_width. For %, uses ref_width.
// Returns std::nullopt if the value can't be parsed.
static std::optional<int> parse_css_length(const std::string& value, uint16_t glyph_width, uint16_t ref_width) {
  if (value == "0" || value == "auto")
    return 0;
  char* end = nullptr;
  if (value.size() > 2 && value.substr(value.size() - 2) == "px") {
    long v = std::strtol(value.c_str(), &end, 10);
    if (end != value.c_str())
      return static_cast<int>(v);
  } else if (value.size() > 2 && value.substr(value.size() - 2) == "pt") {
    float v = std::strtof(value.c_str(), &end);
    if (end != value.c_str())
      return static_cast<int>(v * 4 / 3 + 0.5f);
  } else if (value.size() > 3 && value.substr(value.size() - 3) == "rem") {
    float v = std::strtof(value.c_str(), &end);
    if (end != value.c_str())
      return static_cast<int>(v * glyph_width + 0.5f);
  } else if (value.size() > 2 && value.substr(value.size() - 2) == "em") {
    float v = std::strtof(value.c_str(), &end);
    if (end != value.c_str())
      return static_cast<int>(v * glyph_width + 0.5f);
  } else if (value.size() > 1 && value.back() == '%') {
    float v = std::strtof(value.c_str(), &end);
    if (end != value.c_str())
      return static_cast<int>(v * ref_width / 100 + 0.5f);
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// CSS shorthand value splitter (shared by margin/padding shorthand)
// Splits "10px 20px" → {"10px", "20px"}
// ---------------------------------------------------------------------------

struct FourSides {
  int top = 0, right = 0, bottom = 0, left = 0;
};

static std::vector<std::string> split_css_values(const std::string& value) {
  std::vector<std::string> parts;
  size_t p = 0;
  while (p < value.size()) {
    while (p < value.size() && std::isspace(static_cast<unsigned char>(value[p])))
      ++p;
    size_t start = p;
    while (p < value.size() && !std::isspace(static_cast<unsigned char>(value[p])))
      ++p;
    if (p > start)
      parts.push_back(value.substr(start, p - start));
  }
  return parts;
}

static FourSides parse_shorthand_sides(const std::vector<std::string>& parts, uint16_t glyph_width,
                                       uint16_t ref_width) {
  auto to_px = [&](const std::string& v) -> int {
    auto len = parse_css_length(v, glyph_width, ref_width);
    return len.value_or(0);
  };
  FourSides s;
  if (parts.size() == 1) {
    s.top = s.right = s.bottom = s.left = to_px(parts[0]);
  } else if (parts.size() == 2) {
    s.top = s.bottom = to_px(parts[0]);
    s.left = s.right = to_px(parts[1]);
  } else if (parts.size() == 3) {
    s.top = to_px(parts[0]);
    s.left = s.right = to_px(parts[1]);
    s.bottom = to_px(parts[2]);
  } else if (parts.size() >= 4) {
    s.top = to_px(parts[0]);
    s.right = to_px(parts[1]);
    s.bottom = to_px(parts[2]);
    s.left = to_px(parts[3]);
  }
  return s;
}

// ---------------------------------------------------------------------------
// CssRule
// ---------------------------------------------------------------------------

CssRule CssRule::parse(const char* decl, size_t length, const CssConfig& config) {
  CssRule rule;
  std::string s(decl, length);
  // Lowercase
  for (auto& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  size_t pos = 0;
  while (pos < s.size()) {
    size_t semi = s.find(';', pos);
    if (semi == std::string::npos)
      semi = s.size();

    size_t colon = s.find(':', pos);
    if (colon != std::string::npos && colon < semi) {
      // Extract key and value
      size_t key_start = pos;
      while (key_start < colon && std::isspace(static_cast<unsigned char>(s[key_start])))
        ++key_start;
      size_t key_end = colon;
      while (key_end > key_start && std::isspace(static_cast<unsigned char>(s[key_end - 1])))
        --key_end;

      size_t val_start = colon + 1;
      while (val_start < semi && std::isspace(static_cast<unsigned char>(s[val_start])))
        ++val_start;
      size_t val_end = semi;
      while (val_end > val_start && std::isspace(static_cast<unsigned char>(s[val_end - 1])))
        --val_end;

      std::string key = s.substr(key_start, key_end - key_start);
      std::string value = s.substr(val_start, val_end - val_start);

      if (key == "text-align") {
        if (value == "start" || value == "left")
          rule.set_alignment(Alignment::Start);
        else if (value == "end" || value == "right")
          rule.set_alignment(Alignment::End);
        else if (value == "center")
          rule.set_alignment(Alignment::Center);
        else if (value == "justify")
          rule.set_alignment(Alignment::Justify);
      } else if (key == "font-style") {
        if (value == "normal")
          rule.set_italic(false);
        else if (value == "italic")
          rule.set_italic(true);
      } else if (key == "font-weight") {
        if (value == "normal")
          rule.set_bold(false);
        else if (value == "bold")
          rule.set_bold(true);
      } else if (key == "text-indent") {
        auto len = parse_css_length(value, config.glyph_width, config.content_width);
        if (len.has_value())
          rule.set_indent(static_cast<int16_t>(*len));
      } else if (key == "margin-left") {
        auto len = parse_css_length(value, config.glyph_width, config.content_width);
        if (len.has_value() && *len > 0)
          rule.set_margin_left(static_cast<uint16_t>(*len));
      } else if (key == "margin-right") {
        auto len = parse_css_length(value, config.glyph_width, config.content_width);
        if (len.has_value() && *len > 0)
          rule.set_margin_right(static_cast<uint16_t>(*len));
      } else if (key == "margin-top") {
        auto len = parse_css_length(value, config.glyph_width, config.content_width);
        if (len.has_value())
          rule.set_margin_top(static_cast<uint16_t>(std::max(0, *len)));
      } else if (key == "margin-bottom") {
        auto len = parse_css_length(value, config.glyph_width, config.content_width);
        if (len.has_value())
          rule.set_margin_bottom(static_cast<uint16_t>(std::max(0, *len)));
      } else if (key == "margin") {
        auto parts = split_css_values(value);
        auto s = parse_shorthand_sides(parts, config.glyph_width, config.content_width);
        if (s.left > 0)
          rule.set_margin_left(static_cast<uint16_t>(s.left));
        if (s.right > 0)
          rule.set_margin_right(static_cast<uint16_t>(s.right));
        if (!parts.empty()) {
          rule.set_margin_top(static_cast<uint16_t>(std::max(0, s.top)));
          rule.set_margin_bottom(static_cast<uint16_t>(std::max(0, s.bottom)));
        }
      } else if (key == "padding-left") {
        auto len = parse_css_length(value, config.glyph_width, config.content_width);
        if (len.has_value()) {
          uint16_t val = *len > 0 ? static_cast<uint16_t>(*len) : 0;
          rule.set_margin_left(rule.has_margin_left_ ? rule.margin_left + val : val);
        }
      } else if (key == "padding-right") {
        auto len = parse_css_length(value, config.glyph_width, config.content_width);
        if (len.has_value() && *len > 0) {
          uint16_t val = static_cast<uint16_t>(*len);
          rule.set_margin_right(rule.margin_right_opt().value_or(0) + val);
        }
      } else if (key == "padding-top") {
        auto len = parse_css_length(value, config.glyph_width, config.content_width);
        if (len.has_value() && *len >= 0) {
          uint16_t val = static_cast<uint16_t>(std::max(0, *len));
          rule.set_margin_top(std::max(rule.margin_top_opt().value_or(0), val));
        }
      } else if (key == "padding-bottom") {
        auto len = parse_css_length(value, config.glyph_width, config.content_width);
        if (len.has_value() && *len >= 0) {
          uint16_t val = static_cast<uint16_t>(std::max(0, *len));
          rule.set_margin_bottom(std::max(rule.margin_bottom_opt().value_or(0), val));
        }
      } else if (key == "padding") {
        auto parts = split_css_values(value);
        auto s = parse_shorthand_sides(parts, config.glyph_width, config.content_width);
        if (!parts.empty()) {
          uint16_t lv = s.left > 0 ? static_cast<uint16_t>(s.left) : 0;
          rule.set_margin_left(rule.has_margin_left_ ? rule.margin_left + lv : lv);
        }
        if (s.right > 0)
          rule.set_margin_right(rule.margin_right_opt().value_or(0) + static_cast<uint16_t>(s.right));
        if (s.top > 0)
          rule.set_margin_top(std::max(rule.margin_top_opt().value_or(0), static_cast<uint16_t>(s.top)));
        if (s.bottom > 0)
          rule.set_margin_bottom(std::max(rule.margin_bottom_opt().value_or(0), static_cast<uint16_t>(s.bottom)));
      } else if (key == "float") {
        if (value == "left" || value == "right")
          rule.set_is_float(true);
        else if (value == "none")
          rule.set_is_float(false);
      } else if (key == "display") {
        if (value == "none")
          rule.set_is_hidden(true);
      } else if (key == "border-top-style") {
        if (value != "none" && value != "hidden")
          rule.set_border_top(true);
        else
          rule.set_border_top(false);
      } else if (key == "border-top") {
        // e.g. "1px solid black" — any non-none value means a visible border
        if (value == "none" || value == "0" || value == "hidden")
          rule.set_border_top(false);
        else if (value.find("solid") != std::string::npos || value.find("dashed") != std::string::npos ||
                 value.find("dotted") != std::string::npos || value.find("double") != std::string::npos)
          rule.set_border_top(true);
      } else if (key == "width") {
        if (!value.empty() && value.back() == '%') {
          char* end = nullptr;
          float v = std::strtof(value.c_str(), &end);
          if (end != value.c_str()) {
            int pct = static_cast<int>(v + 0.5f);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            rule.set_width_pct(static_cast<uint8_t>(pct));
          }
        }
      } else if (key == "page-break-before") {
        if (value == "always" || value == "left" || value == "right")
          rule.set_page_break_before(true);
        else if (value == "auto" || value == "avoid")
          rule.set_page_break_before(false);
      } else if (key == "page-break-after") {
        if (value == "always" || value == "left" || value == "right")
          rule.set_page_break_after(true);
        else if (value == "auto" || value == "avoid")
          rule.set_page_break_after(false);
      } else if (key == "text-transform") {
        if (value == "uppercase")
          rule.set_text_transform(TextTransform::Uppercase);
        else if (value == "lowercase")
          rule.set_text_transform(TextTransform::Lowercase);
        else if (value == "capitalize")
          rule.set_text_transform(TextTransform::Capitalize);
        else if (value == "none")
          rule.set_text_transform(TextTransform::None);
      } else if (key == "font-variant") {
        if (value == "small-caps") {
          rule.set_font_variant_small_caps(true);
          // Approximate as uppercase when no explicit text-transform is set
          if (!rule.has_text_transform_)
            rule.set_text_transform(TextTransform::Uppercase);
        }
      } else if (key == "vertical-align") {
        // CSS super/sub -> font_size_pct: 75% + VerticalAlign
        if (value == "super") {
          if (!rule.has_font_size_pct_)
            rule.set_font_size_pct(75);
          rule.set_vertical_align(VerticalAlign::Super);
        } else if (value == "sub") {
          if (!rule.has_font_size_pct_)
            rule.set_font_size_pct(75);
          rule.set_vertical_align(VerticalAlign::Sub);
        } else if (value == "top" || value == "bottom") {
          if (!rule.has_font_size_pct_)
            rule.set_font_size_pct(75);
        }
      } else if (key == "line-height") {
        // Parse line-height as percentage of our natural y_advance.
        // Our fonts have y_advance ≈ 1.5× em-size, so CSS line-height: 1.5 = 100% (use y_advance as-is).
        // CSS line-height: 1.2 (browser normal) = 80% (slightly tighter than y_advance).
        // Common values: "normal", "1.2", "1.5", "140%", "1.4em"
        static constexpr float kNormFactor = 1.5f;
        char* end = nullptr;
        if (value == "normal" || value == "inherit") {
          rule.set_line_height_pct(100);
        } else if (value.size() > 1 && value.back() == '%') {
          float pct = std::strtof(value.c_str(), &end);
          if (end != value.c_str()) {
            uint8_t val = static_cast<uint8_t>(std::clamp(pct / kNormFactor, 70.0f, 200.0f));
            rule.set_line_height_pct(val);
          }
        } else if (value.size() > 2 && value.substr(value.size() - 2) == "em") {
          float em = std::strtof(value.c_str(), &end);
          if (end != value.c_str()) {
            uint8_t val = static_cast<uint8_t>(std::clamp(em * 100.0f / kNormFactor, 70.0f, 200.0f));
            rule.set_line_height_pct(val);
          }
        } else {
          // Unitless number (e.g. "1.5")
          float num = std::strtof(value.c_str(), &end);
          if (end != value.c_str()) {
            uint8_t val = static_cast<uint8_t>(std::clamp(num * 100.0f / kNormFactor, 70.0f, 200.0f));
            rule.set_line_height_pct(val);
          }
        }
      } else if (key == "list-style-type" || key == "list-style") {
        if (value == "none")
          rule.set_list_style_none(true);
      } else if (key == "font-size") {
        if (value == "small" || value == "x-small" || value == "xx-small" || value == "smaller")
          rule.set_font_size_pct(80);
        else if (value == "large" || value == "larger")
          rule.set_font_size_pct(120);
        else if (value == "x-large")
          rule.set_font_size_pct(140);
        else if (value == "xx-large")
          rule.set_font_size_pct(160);
        else if (value == "medium" || value == "normal")
          rule.set_font_size_pct(100);
        else {
          // Try parsing numeric values: percentages (90%) and em (0.9em)
          char* end = nullptr;
          if (value.size() > 1 && value.back() == '%') {
            float pct = std::strtof(value.c_str(), &end);
            if (end != value.c_str()) {
              rule.set_font_size_pct(static_cast<uint8_t>(std::clamp(pct, 30.0f, 250.0f)));
            }
          } else if (value.size() > 2 && value.substr(value.size() - 2) == "em") {
            float em = std::strtof(value.c_str(), &end);
            if (end != value.c_str()) {
              rule.set_font_size_pct(static_cast<uint8_t>(std::clamp(em * 100.0f, 30.0f, 250.0f)));
            }
          } else if (value.size() > 3 && value.substr(value.size() - 3) == "rem") {
            float rem = std::strtof(value.c_str(), &end);
            if (end != value.c_str()) {
              rule.set_font_size_pct(static_cast<uint8_t>(std::clamp(rem * 100.0f, 30.0f, 250.0f)));
            }
          } else if (value.size() > 2 && value.substr(value.size() - 2) == "pt") {
            float pt = std::strtof(value.c_str(), &end);
            if (end != value.c_str()) {
              float ratio = pt / 12.0f;
              rule.set_font_size_pct(static_cast<uint8_t>(std::clamp(ratio * 100.0f, 30.0f, 250.0f)));
            }
          } else if (value.size() > 2 && value.substr(value.size() - 2) == "px") {
            float px = std::strtof(value.c_str(), &end);
            if (end != value.c_str()) {
              float ratio = px / 24.0f;
              rule.set_font_size_pct(static_cast<uint8_t>(std::clamp(ratio * 100.0f, 30.0f, 250.0f)));
            }
          }
        }
      }
    }
    pos = semi + 1;
  }

  // If both margins are set in the same rule, clamp their total to
  // max_margin_pct% of content_width, scaling proportionally.
  if (rule.has_margin_left_ && rule.has_margin_right_) {
    uint16_t total = rule.margin_left + rule.margin_right;
    uint16_t max_total = config.max_margin_pct * config.content_width / 100;
    if (total > max_total && total > 0) {
      float scale = static_cast<float>(max_total) / total;
      rule.set_margin_left(static_cast<uint16_t>(rule.margin_left * scale));
      rule.set_margin_right(static_cast<uint16_t>(rule.margin_right * scale));
    }
  }

  return rule;
}

CssRule CssRule::operator+(const CssRule& rhs) const {
  CssRule result;

  if (rhs.has_alignment_)
    result.set_alignment(rhs.alignment);
  else if (has_alignment_)
    result.set_alignment(alignment);
  if (rhs.has_italic_)
    result.set_italic(rhs.italic);
  else if (has_italic_)
    result.set_italic(italic);
  if (rhs.has_bold_)
    result.set_bold(rhs.bold);
  else if (has_bold_)
    result.set_bold(bold);
  if (rhs.has_indent_)
    result.set_indent(rhs.indent);
  else if (has_indent_)
    result.set_indent(indent);
  if (rhs.has_font_size_pct_)
    result.set_font_size_pct(rhs.font_size_pct);
  else if (has_font_size_pct_)
    result.set_font_size_pct(font_size_pct);
  if (rhs.has_margin_left_)
    result.set_margin_left(rhs.margin_left);
  else if (has_margin_left_)
    result.set_margin_left(margin_left);
  if (rhs.has_margin_right_)
    result.set_margin_right(rhs.margin_right);
  else if (has_margin_right_)
    result.set_margin_right(margin_right);
  if (rhs.has_margin_top_)
    result.set_margin_top(rhs.margin_top);
  else if (has_margin_top_)
    result.set_margin_top(margin_top);
  if (rhs.has_margin_bottom_)
    result.set_margin_bottom(rhs.margin_bottom);
  else if (has_margin_bottom_)
    result.set_margin_bottom(margin_bottom);
  if (rhs.has_is_float_)
    result.set_is_float(rhs.is_float);
  else if (has_is_float_)
    result.set_is_float(is_float);
  if (rhs.has_is_hidden_)
    result.set_is_hidden(rhs.is_hidden);
  else if (has_is_hidden_)
    result.set_is_hidden(is_hidden);
  if (rhs.has_page_break_before_)
    result.set_page_break_before(rhs.page_break_before);
  else if (has_page_break_before_)
    result.set_page_break_before(page_break_before);
  if (rhs.has_page_break_after_)
    result.set_page_break_after(rhs.page_break_after);
  else if (has_page_break_after_)
    result.set_page_break_after(page_break_after);
  if (rhs.has_text_transform_)
    result.set_text_transform(rhs.text_transform);
  else if (has_text_transform_)
    result.set_text_transform(text_transform);
  if (rhs.has_vertical_align_)
    result.set_vertical_align(rhs.vertical_align);
  else if (has_vertical_align_)
    result.set_vertical_align(vertical_align);
  if (rhs.has_line_height_pct_)
    result.set_line_height_pct(rhs.line_height_pct);
  else if (has_line_height_pct_)
    result.set_line_height_pct(line_height_pct);
  if (rhs.has_list_style_none_)
    result.set_list_style_none(rhs.list_style_none);
  else if (has_list_style_none_)
    result.set_list_style_none(list_style_none);
  if (rhs.has_font_variant_small_caps_)
    result.set_font_variant_small_caps(rhs.font_variant_small_caps);
  else if (has_font_variant_small_caps_)
    result.set_font_variant_small_caps(font_variant_small_caps);
  if (rhs.has_border_top_)
    result.set_border_top(rhs.border_top);
  else if (has_border_top_)
    result.set_border_top(border_top);
  if (rhs.has_width_pct_)
    result.set_width_pct(rhs.width_pct);
  else if (has_width_pct_)
    result.set_width_pct(width_pct);

  return result;
}

// ---------------------------------------------------------------------------
// CssStylesheet::Selector
// ---------------------------------------------------------------------------

bool CssStylesheet::Selector::try_parse(const char* s, size_t len, Selector& out) {
  out = {};
  // Trim
  while (len > 0 && std::isspace(static_cast<unsigned char>(s[0]))) {
    ++s;
    --len;
  }
  while (len > 0 && std::isspace(static_cast<unsigned char>(s[len - 1])))
    --len;

  if (len == 0)
    return false;

  // Reject complex selectors (contain spaces, >, +, ~, :, [)
  for (size_t i = 0; i < len; ++i) {
    char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '+' || c == '~' || c == ':' || c == '[')
      return false;
  }

  // Parse compound selector: element.class#id etc
  char kind = 'e';
  std::string current;

  for (size_t i = 0; i < len; ++i) {
    char c = s[i];
    if (c == '.' || c == '#') {
      if (!current.empty()) {
        if (kind == 'e')
          out.element = std::move(current);
        else if (kind == '.')
          out.classes.push_back(std::move(current));
        else if (kind == '#')
          out.id = std::move(current);
      }
      current.clear();
      kind = c;
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    if (kind == 'e')
      out.element = std::move(current);
    else if (kind == '.')
      out.classes.push_back(std::move(current));
    else if (kind == '#')
      out.id = std::move(current);
  }

  return !out.element.empty() || !out.id.empty() || !out.classes.empty();
}

bool CssStylesheet::Selector::matches(const char* element, const char* id,
                                      const std::vector<std::string>& classes) const {
  if (!this->element.empty() && (element == nullptr || this->element != element))
    return false;
  if (!this->id.empty()) {
    if (id == nullptr || this->id != id)
      return false;
  }
  for (const auto& cls : this->classes) {
    bool found = false;
    for (const auto& c : classes) {
      if (c == cls) {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

uint32_t CssStylesheet::Selector::specificity() const {
  uint32_t ids = id.empty() ? 0 : 1;
  uint32_t cls = static_cast<uint32_t>(classes.size());
  uint32_t elems = element.empty() ? 0 : 1;
  return (ids << 16) | (cls << 8) | elems;
}

// ---------------------------------------------------------------------------
// CssStylesheet
// ---------------------------------------------------------------------------

std::string CssStylesheet::filter_comments(const char* css, size_t length) {
  std::string result;
  result.reserve(length);
  size_t i = 0;
  while (i < length) {
    if (i + 1 < length && css[i] == '/' && css[i + 1] == '*') {
      i += 2;
      while (i + 1 < length) {
        if (css[i] == '*' && css[i + 1] == '/') {
          i += 2;
          break;
        }
        ++i;
      }
    } else {
      result += css[i];
      ++i;
    }
  }
  return result;
}

void CssStylesheet::extend_from_sheet(const char* css, size_t length) {
  std::string sheet_copy(css, length);
  extend_from_mut_sheet(sheet_copy.data(), sheet_copy.size());
}

void CssStylesheet::extend_from_mut_sheet(char* css, size_t length) {
  size_t rd = 0, wr = 0;
  while (rd < length) {
    if (rd + 1 < length && css[rd] == '/' && css[rd + 1] == '*') {
      rd += 2;
      while (rd + 1 < length) {
        if (css[rd] == '*' && css[rd + 1] == '/') {
          rd += 2;
          break;
        }
        ++rd;
      }
    } else {
      css[wr++] = css[rd++];
    }
  }

  std::string_view sheet(css, wr);

  size_t pos = 0;
  while (pos < sheet.size()) {
    // Find next '{' or '@'
    size_t brace = std::string::npos;
    size_t at = std::string::npos;
    for (size_t i = pos; i < sheet.size(); ++i) {
      if (sheet[i] == '{' || sheet[i] == '@') {
        if (sheet[i] == '@')
          at = i;
        else
          brace = i;
        break;
      }
    }

    // Handle at-rules
    if (at != std::string::npos && (brace == std::string::npos || at < brace)) {
      // Skip @-rule: find ';' or '{...}'
      size_t semi = sheet.find(';', at);
      size_t ob = sheet.find('{', at);
      if (semi != std::string::npos && (ob == std::string::npos || semi < ob)) {
        pos = semi + 1;
      } else if (ob != std::string::npos) {
        // Find matching '}'
        int depth = 1;
        size_t j = ob + 1;
        while (j < sheet.size() && depth > 0) {
          if (sheet[j] == '{')
            ++depth;
          else if (sheet[j] == '}')
            --depth;
          ++j;
        }
        pos = j;
      } else {
        break;
      }
      continue;
    }

    if (brace == std::string::npos)
      break;

    // Find closing '}'
    int depth = 1;
    size_t end_pos = brace + 1;
    while (end_pos < sheet.size() && depth > 0) {
      if (sheet[end_pos] == '{')
        ++depth;
      else if (sheet[end_pos] == '}')
        --depth;
      ++end_pos;
    }
    if (depth != 0)
      break;
    --end_pos;  // point at '}'

    std::string declarations(sheet.substr(brace + 1, end_pos - brace - 1));

    // Skip nested blocks
    if (declarations.find('{') == std::string::npos) {
      CssRule rule = CssRule::parse(declarations.c_str(), declarations.size(), config_);
      if (rule.has_any()) {
        std::string selectors(sheet.substr(pos, brace - pos));
        // Split by comma
        size_t sp = 0;
        while (sp < selectors.size()) {
          size_t comma = selectors.find(',', sp);
          if (comma == std::string::npos)
            comma = selectors.size();

          Selector sel;
          if (Selector::try_parse(selectors.c_str() + sp, comma - sp, sel)) {
            rules_.emplace_back(std::move(sel), rule);
          }
          sp = comma + 1;
        }
      }
    }

    pos = end_pos + 1;
  }
}

// Check if a whitespace-separated class string contains a specific class.
static bool class_list_contains(const char* cls, const std::string& target) {
  if (!cls || target.empty())
    return false;
  size_t tlen = target.size();
  const char* p = cls;
  while (*p) {
    while (*p && std::isspace(static_cast<unsigned char>(*p)))
      ++p;
    const char* start = p;
    while (*p && !std::isspace(static_cast<unsigned char>(*p)))
      ++p;
    size_t len = static_cast<size_t>(p - start);
    if (len == tlen && std::memcmp(start, target.c_str(), len) == 0)
      return true;
  }
  return false;
}

// Match a selector against element/id/class without allocating vectors.
static bool selector_matches_raw(const CssStylesheet::Selector& sel, const char* element, const char* id,
                                 const char* cls) {
  if (!sel.element.empty() && (element == nullptr || sel.element != element))
    return false;
  if (!sel.id.empty()) {
    if (id == nullptr || sel.id != id)
      return false;
  }
  for (const auto& c : sel.classes) {
    if (!class_list_contains(cls, c))
      return false;
  }
  return true;
}

// Length-based class list check: does whitespace-separated cls contain target?
static bool class_list_contains_n(const char* cls, size_t cls_len, const std::string& target) {
  if (!cls || cls_len == 0 || target.empty())
    return false;
  size_t tlen = target.size();
  const char* end = cls + cls_len;
  const char* p = cls;
  while (p < end) {
    while (p < end && std::isspace(static_cast<unsigned char>(*p)))
      ++p;
    const char* start = p;
    while (p < end && !std::isspace(static_cast<unsigned char>(*p)))
      ++p;
    size_t len = static_cast<size_t>(p - start);
    if (len == tlen && std::memcmp(start, target.c_str(), len) == 0)
      return true;
  }
  return false;
}

static bool selector_matches_n(const CssStylesheet::Selector& sel, const char* element, size_t element_len,
                               const char* id, size_t id_len, const char* cls, size_t cls_len) {
  if (!sel.element.empty()) {
    if (element_len != sel.element.size() || std::memcmp(element, sel.element.c_str(), element_len) != 0)
      return false;
  }
  if (!sel.id.empty()) {
    if (id_len != sel.id.size() || std::memcmp(id, sel.id.c_str(), id_len) != 0)
      return false;
  }
  for (const auto& c : sel.classes) {
    if (!class_list_contains_n(cls, cls_len, c))
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// get() overloads: scan all rules, match selectors, sort, merge.
// ---------------------------------------------------------------------------

CssRule CssStylesheet::get(const char* element, const char* id, const char* cls) const {
  if (rules_.empty())
    return {};

  size_t el_len = element ? std::strlen(element) : 0;
  size_t id_len = id ? std::strlen(id) : 0;
  size_t cls_len = cls ? std::strlen(cls) : 0;
  return get(element ? element : "", el_len, id, id_len, cls, cls_len);
}

CssRule CssStylesheet::get(const char* element, size_t element_len, const char* id, size_t id_len, const char* cls,
                           size_t cls_len) const {
  if (rules_.empty())
    return {};
  struct Match {
    uint32_t specificity;
    size_t index;
    const CssRule* rule;
  };
  Match inline_buf[8];
  size_t match_count = 0;
  bool used_heap = false;
  std::vector<Match> heap_matches;

  for (size_t i = 0; i < rules_.size(); ++i) {
    if (selector_matches_n(rules_[i].first, element, element_len, id, id_len, cls, cls_len)) {
      Match m{rules_[i].first.specificity(), i, &rules_[i].second};
      if (!used_heap && match_count < 8) {
        inline_buf[match_count++] = m;
      } else {
        if (!used_heap) {
          heap_matches.assign(inline_buf, inline_buf + match_count);
          used_heap = true;
        }
        heap_matches.push_back(m);
        match_count = heap_matches.size();
      }
    }
  }

  if (match_count == 0)
    return {};

  Match* matches = used_heap ? heap_matches.data() : inline_buf;
  std::sort(matches, matches + match_count, [](const Match& a, const Match& b) {
    if (a.specificity != b.specificity)
      return a.specificity < b.specificity;
    return a.index < b.index;
  });

  CssRule result;
  for (size_t i = 0; i < match_count; ++i) {
    result = result + *matches[i].rule;
  }
  return result;
}

}  // namespace microreader
