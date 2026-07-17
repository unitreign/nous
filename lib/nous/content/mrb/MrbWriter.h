#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "../ContentModel.h"
#include "MrbFormat.h"

namespace microreader {

// Batches small writes into a memory buffer (4 KB), flushing to the
// underlying FILE* only when full or on seek.  tell() is O(1).
class BufferedFileWriter {
 public:
  BufferedFileWriter() = default;
  ~BufferedFileWriter() {
    close();
  }
  BufferedFileWriter(const BufferedFileWriter&) = delete;
  BufferedFileWriter& operator=(const BufferedFileWriter&) = delete;

  bool open(const char* path);
  void close();
  bool write(const void* data, size_t size);
  bool seek(uint32_t offset);
  bool flush();
  uint32_t tell() const {
    return pos_;
  }
  bool is_open() const {
    return f_ != nullptr;
  }

 private:
  static constexpr size_t kBufSize = 4096;
  FILE* f_ = nullptr;
  uint32_t pos_ = 0;
  size_t used_ = 0;
  uint8_t buf_[kBufSize];
};

// Writes an MRB file sequentially.  Usage:
//
//   MrbWriter w;
//   w.open("book.mrb");
//   w.begin_chapter();
//   w.write_paragraph(para);
//   w.end_chapter();
//   w.finish(metadata, toc);
//
class MrbWriter {
 public:
  MrbWriter() = default;
  ~MrbWriter() {
    close();
  }

  MrbWriter(const MrbWriter&) = delete;
  MrbWriter& operator=(const MrbWriter&) = delete;

  bool open(const char* path);
  void close();

  // Call before writing paragraphs for a new chapter.
  void begin_chapter();

  // Write one paragraph.  Returns false on I/O error.
  bool write_paragraph(const Paragraph& para);

  // Write a text paragraph from a raw run slice — avoids building an intermediate
  // Paragraph with a heap-allocated runs vector.  Used by the split-write path.
  bool write_text_paragraph(const TextParagraph& meta, uint16_t spacing, const Run* runs, size_t run_count);

  // Call after writing all paragraphs for a chapter.
  void end_chapter();

  // Add an image reference (dimensions pre-resolved from EPUB).
  // Returns the image index to use in paragraph image refs.
  uint16_t add_image_ref(uint32_t local_header_offset, uint16_t width, uint16_t height);

  // Update the size of an existing image ref (used by MRB converter after
  // lazy resolution).  No-op if idx is out of range.
  void update_image_size(uint16_t idx, uint16_t width, uint16_t height);

  // Returns true if the image ref at idx has non-zero dimensions.
  bool image_size_known(uint16_t idx) const {
    return idx < images_.size() && (images_[idx].width != 0 || images_[idx].height != 0);
  }

  // Add an anchor (id → paragraph) for runtime link fragment navigation.
  // Call after all chapters are written, before finish().
  void add_anchor(uint16_t chapter_idx, uint16_t para_index, const char* id, size_t id_len);

  // Finalize: write index tables, metadata, TOC, spine file table, and fix up header.
  // spine_files: base filenames of each spine item (index = chapter index).
  bool finish(const EpubMetadata& meta, const TableOfContents& toc, const std::vector<std::string>& spine_files = {});

 private:
  BufferedFileWriter bw_;
  uint32_t paragraph_count_ = 0;
  std::vector<MrbChapterEntry> chapters_;
  std::vector<MrbImageRef> images_;
  bool in_chapter_ = false;

  // Per-chapter state
  uint16_t chapter_para_count_ = 0;  // paragraph count in current chapter
  uint32_t chapter_char_count_ = 0;  // total text chars (bytes) in current chapter

  // Descriptor table: streamed to a single temp file for the whole book.
  // Each 8-byte entry {file_offset(u32), char_offset(u32)} is appended as paragraphs
  // are written. end_chapter() seeks back to chapter_desc_start_ and copies exactly
  // chapter_para_count_*8 bytes into the MRB. Zero heap allocation, one FD, unbounded.
  FILE* desc_tmp_ = nullptr;
  char desc_tmp_path_[260] = {};
  uint32_t chapter_desc_start_ = 0;  // byte offset in desc_tmp_ where current chapter begins

  // Anchor table: streamed directly to a temp file during conversion to avoid
  // large contiguous RAM allocation. Copied into the MRB at finish().
  FILE* anchor_tmp_ = nullptr;
  char anchor_tmp_path_[260] = {};
  uint32_t anchor_count_ = 0;

  // Write raw bytes.
  bool write_bytes(const void* data, size_t size);

  // Write a length-prefixed UTF-8 string (uint16 length + bytes).
  bool write_string(const std::string& s);
};

}  // namespace microreader
