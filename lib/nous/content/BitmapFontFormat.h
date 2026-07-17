#pragma once

// MBF (Microreader Bitmap Font) binary format — version 4.
//
// Each MBF file contains one font at one pixel size, with up to 4 styles
// (Regular + optional Bold, Italic, BoldItalic) and optional 2-bit grayscale
// data for antialiased rendering on e-ink displays.
//
// Layout (single-style, style_flags=0):
//   [MbfHeader]                     40 bytes
//   [MbfRange × num_ranges]         8 bytes each
//   [MbfGlyph × num_glyphs]        10 bytes each
//   [BW bitmap data]               variable (1-bit packed, MSB-first)
//   [Gray LSB bitmap data]         variable (same size as BW; absent if gray_lsb_offset=0)
//   [Gray MSB bitmap data]         variable (same size as BW; absent if gray_msb_offset=0)
//
// Grayscale: each glyph has 3 bitmap sections (BW, LSB, MSB) all sharing the
// same per-glyph bitmap_offset. The offset is relative to bitmap_data_offset
// for BW, gray_lsb_offset for LSB, gray_msb_offset for MSB.
// 2-bit gray value = (MSB << 1) | LSB:
//   0 = white, 1 = light gray, 2 = gray, 3 = dark gray.
//
// Multi-style (style_flags != 0): additional style sections are appended
// between the Regular glyph table and the bitmap data. The header fields
// bold_offset / italic_offset / bold_italic_offset point to MbfStyleSection
// structs (4 bytes each) followed by their own MbfRange[] + MbfGlyph[] tables.
// All bitmap_offset values are relative to bitmap_data_offset (shared pool).
//
// Multiple sizes (Small/Normal/Large) are separate MBF files loaded into
// a BitmapFontSet at runtime.
//
// Glyph lookup: binary-search ranges by codepoint → index into glyph table.
// Bitmap data: each glyph has ceil(bitmap_width/8) × bitmap_height bytes.
// Bit polarity: 1 = white, 0 = black (bit CLEAR = draw pixel).

#include <cstddef>
#include <cstdint>

namespace microreader {

static constexpr uint32_t kMbfMagic = 0x3446424D;  // "MBF4" little-endian
static constexpr uint8_t kMbfVersion = 4;

#pragma pack(push, 1)

// File header — 50 bytes, all fields little-endian.
struct MbfHeader {
  uint32_t magic;               //  0: must be kMbfMagic
  uint8_t version;              //  4: must be kMbfVersion
  uint8_t glyph_height;         //  5: maximum rendered glyph height (px)
  uint8_t baseline;             //  6: distance from line top to baseline (px)
  uint8_t y_advance;            //  7: line height / vertical advance (px)
  uint8_t default_advance;      //  8: fallback xAdvance for missing glyphs
  uint8_t style_flags;          //  9: bitmask of present styles (bit0=Regular, bit1=Bold, bit2=Italic, bit3=BoldItalic)
  uint16_t num_ranges;          // 10: number of MbfRange entries (Regular style)
  uint16_t num_glyphs;          // 12: total glyph count (Regular style)
  uint16_t nominal_size;        // 14: nominal pixel size requested at generation
  uint32_t kerning_length;      // 16: byte length of kerning data (Regular style)
  uint32_t bitmap_data_offset;  // 20: byte offset from file start to BW bitmap data (shared by all styles)
  uint32_t bold_offset;         // 24: file offset to Bold MbfStyleSection (0 = absent)
  uint32_t italic_offset;       // 28: file offset to Italic MbfStyleSection (0 = absent)
  uint32_t bold_italic_offset;  // 32: file offset to BoldItalic MbfStyleSection (0 = absent)
  uint32_t kerning_offset;      // 36: file offset to Regular style kerning data
  uint32_t gray_lsb_offset;     // 40: file offset to grayscale LSB bitmap data (0 = absent)
  uint32_t gray_msb_offset;     // 44: file offset to grayscale MSB bitmap data (0 = absent)
  int8_t underline_pos;         // 48: distance below baseline to underline top (px, positive = below)
  uint8_t underline_thickness;  // 49: underline height in pixels (>= 1)
};
static_assert(sizeof(MbfHeader) == 50, "MbfHeader must be 50 bytes");

// Style section preamble — appears at bold_offset / italic_offset / bold_italic_offset.
// Immediately followed by MbfRange[num_ranges] then MbfGlyph[num_glyphs] then MbfClassKerning data block.
struct MbfStyleSection {
  uint16_t num_ranges;
  uint16_t num_glyphs;
  uint32_t kerning_length;
};
static_assert(sizeof(MbfStyleSection) == 8, "MbfStyleSection must be 8 bytes");

// Unicode range — maps a contiguous codepoint block to a slice of the glyph table.
// 8 bytes each.
struct MbfRange {
  uint32_t first_codepoint;    // first Unicode codepoint in this range
  uint16_t count;              // number of consecutive codepoints
  uint16_t glyph_table_start;  // index of the first glyph in the glyph table
};
static_assert(sizeof(MbfRange) == 8, "MbfRange must be 8 bytes");

// Per-glyph metrics + bitmap location — 10 bytes each.
struct MbfGlyph {
  uint32_t bitmap_offset;  // byte offset into bitmap data section
  uint8_t advance_width;   // horizontal advance (cursor movement)
  uint8_t bitmap_width;    // bitmap width in pixels (0 for space-like glyphs)
  uint8_t bitmap_height;   // bitmap height in pixels
  int8_t x_offset;         // horizontal offset from cursor to bitmap left edge
  int8_t y_offset;         // vertical offset from baseline to bitmap top (negative = above)
  uint8_t reserved;
};
static_assert(sizeof(MbfGlyph) == 10, "MbfGlyph must be 10 bytes");

// Class kerning metadata block (Format 3).
// The binary kerning section is laid out as follows:
//   uint8_t num_l_classes_minus_1;
//   uint8_t num_r_classes_minus_1;
//   uint8_t l_class_map[num_glyphs];
//   uint8_t r_class_map[num_glyphs];
//   int8_t  matrix[num_l_classes * num_r_classes];
//
struct MbfClassKerning {
  uint8_t num_l_classes_minus_1;
  uint8_t num_r_classes_minus_1;
  // Followed by variable length arrays:
  // uint8_t l_class_map[num_glyphs];
  // uint8_t r_class_map[num_glyphs];
  // int8_t  matrix[ (num_l_classes_minus_1+1) * (num_r_classes_minus_1+1) ];
};

#pragma pack(pop)

}  // namespace microreader
