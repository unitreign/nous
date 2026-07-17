#pragma once

// MRB (MicroReader Book) — pre-processed binary format for fast on-device
// EPUB reading.  Stores paragraphs with styling and text inline.
//
// File layout (all values little-endian):
//
//   [Header 32 bytes]
//   [Paragraph data — variable, written sequentially]   ← no prev/next links
//   [Per-chapter descriptor tables — paragraph_count × 8 bytes each]
//   [Chapter table  — chapter_count  × 16 bytes]
//   [Image ref table— image_count    × 8 bytes]
//   [Metadata blob  — variable]
//   [TOC blob       — variable]
//
// Per-chapter descriptor table (written immediately after the chapter's
// last paragraph):
//   N × { file_offset(u32), char_offset(u32) }
// file_offset: absolute position of the paragraph record in the file.
// char_offset: cumulative UTF-8 bytes of text before this paragraph.
// This allows O(1) random access to any paragraph and accurate char-based
// progress without reading any paragraph data.

#include <cstdint>
#include <cstring>

namespace microreader {

// ---------------------------------------------------------------------------
// Magic & version
// ---------------------------------------------------------------------------

static constexpr uint8_t kMrbMagic[4] = {'M', 'R', 'B', '1'};
static constexpr uint16_t kMrbVersion = 11;  // v11: large paragraphs split at conversion time (≤4KB text each)

// ---------------------------------------------------------------------------
// Header (32 bytes, fixed)
// ---------------------------------------------------------------------------

struct MrbHeader {
  uint8_t magic[4];  // "MRB1"
  uint16_t version;  // 2
  uint16_t flags;    // reserved (0)
  uint32_t paragraph_count;
  uint16_t chapter_count;
  uint16_t image_count;
  uint32_t anchor_offset;   // file offset of anchor table (id→para for link navigation)
  uint32_t chapter_offset;  // file offset of chapter table
  uint32_t image_offset;    // file offset of image ref table
  uint32_t meta_offset;     // file offset of metadata blob
};
static_assert(sizeof(MrbHeader) == 32, "MrbHeader must be 32 bytes");

// ---------------------------------------------------------------------------
// Chapter table entry (16 bytes each)
// ---------------------------------------------------------------------------

struct MrbChapterEntry {
  uint32_t para_table_offset;  // file offset of the paragraph descriptor table for this chapter
  uint32_t reserved;           // unused
  uint16_t paragraph_count;
  uint16_t reserved1;
  uint32_t char_count;  // total UTF-8 bytes of text in this chapter
};
static_assert(sizeof(MrbChapterEntry) == 16, "MrbChapterEntry must be 16 bytes");

// ---------------------------------------------------------------------------
// Image reference entry (8 bytes each)
// ---------------------------------------------------------------------------

struct MrbImageRef {
  uint32_t local_header_offset;  // offset to local file header in EPUB ZIP
  uint16_t width;
  uint16_t height;
};
static_assert(sizeof(MrbImageRef) == 8, "MrbImageRef must be 8 bytes");

// ---------------------------------------------------------------------------
// Paragraph type tags (match ParagraphType enum values)
// ---------------------------------------------------------------------------

static constexpr uint8_t kMrbParaText = 0;
static constexpr uint8_t kMrbParaImage = 1;
static constexpr uint8_t kMrbParaHr = 2;
static constexpr uint8_t kMrbParaPageBreak = 3;

// ---------------------------------------------------------------------------
// Sentinel values for optional fields
// ---------------------------------------------------------------------------

static constexpr uint8_t kMrbAlignDefault = 0xFF;
static constexpr int16_t kMrbIndentNone = 0x7FFF;
static constexpr uint16_t kMrbSpacingDefault = 0xFFFF;
static constexpr uint16_t kMrbNoImage = 0xFFFF;
static constexpr uint8_t kMrbHrWidthDefault = 0xFF;

// ---------------------------------------------------------------------------
// Little-endian serialization helpers
// ---------------------------------------------------------------------------

inline void mrb_write_u8(uint8_t* dst, uint8_t v) {
  dst[0] = v;
}
inline void mrb_write_u16(uint8_t* dst, uint16_t v) {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
}
inline void mrb_write_i16(uint8_t* dst, int16_t v) {
  mrb_write_u16(dst, static_cast<uint16_t>(v));
}
inline void mrb_write_u32(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
  dst[2] = static_cast<uint8_t>(v >> 16);
  dst[3] = static_cast<uint8_t>(v >> 24);
}

inline uint8_t mrb_read_u8(const uint8_t* src) {
  return src[0];
}
inline uint16_t mrb_read_u16(const uint8_t* src) {
  return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
}
inline int16_t mrb_read_i16(const uint8_t* src) {
  return static_cast<int16_t>(mrb_read_u16(src));
}
inline uint32_t mrb_read_u32(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) | (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

}  // namespace microreader
