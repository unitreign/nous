#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "XmlReader.h"  // for IXmlInput

namespace microreader {

// Error codes for ZIP operations.
enum class ZipError {
  Ok = 0,
  FileNotFound,
  InvalidSignature,
  InvalidData,
  UnsupportedCompression,
  DecompressionFailed,
  ReadError,
  OutOfMemory,
};

// Metadata for a single file inside a ZIP archive.
// Names are stored in a contiguous blob owned by ZipReader to avoid per-entry
// heap allocations (critical for ESP32 memory budget with large EPUBs).
struct ZipEntry {
  std::string_view name;  // view into ZipReader's name blob
  uint32_t uncompressed_size = 0;
  uint32_t compressed_size = 0;
  uint32_t local_header_offset = 0;  // offset to the local file header
  uint32_t data_offset = 0;          // offset to the first byte of file data (0 = not cached)
  uint16_t compression = 0;          // 0=stored, 8=deflate
};

// Callback that receives decompressed data chunks.
// Return true to continue, false to abort.
using ZipDataCallback = bool (*)(const uint8_t* data, size_t size, void* user_data);

// Abstract file interface for ZIP reading.
// This allows the ZIP reader to work with FILE*, SD cards, or memory buffers.
class IZipFile {
 public:
  virtual ~IZipFile() = default;
  virtual bool seek(int64_t offset, int whence) = 0;  // SEEK_SET, SEEK_CUR, SEEK_END
  virtual int64_t tell() = 0;
  virtual size_t read(void* buf, size_t size) = 0;
};

// Reads a ZIP archive's central directory and provides streaming extraction.
//
// Usage:
//   StdioZipFile file("book.epub");
//   ZipReader reader;
//   if (reader.open(file) == ZipError::Ok) {
//     auto* entry = reader.find("META-INF/container.xml");
//     std::vector<uint8_t> buf;
//     reader.extract(file, *entry, buf);
//   }
class ZipReader {
 public:
  ZipReader() = default;

  // Parse the central directory. Populates entries().
  // Optional work_buf (≥ central-directory size, typically ≤ 48 KB) avoids a
  // heap allocation for the bulk CD read — useful when heap is fragmented.
  ZipError open(IZipFile& file);

  // Number of entries in the archive.
  size_t entry_count() const {
    return entries_.size();
  }

  // Access entries by index.
  const ZipEntry& entry(size_t index) const {
    return entries_[index];
  }
  const std::vector<ZipEntry>& entries() const {
    return entries_;
  }

  // Build a ZipEntry by reading the local file header at `offset`.
  // No central directory needed — reads ~30 bytes from disk.
  // entry.name will be empty (callers should use magic-byte format detection).
  static ZipError read_local_entry(IZipFile& file, uint32_t offset, ZipEntry& out);

  // Find an entry by name. Returns nullptr if not found.
  const ZipEntry* find(const char* name) const;
  const ZipEntry* find(const std::string& name) const {
    return find(name.c_str());
  }

  // Extract an entry into a pre-allocated vector (resized to uncompressed_size).
  ZipError extract(IZipFile& file, const ZipEntry& entry, std::vector<uint8_t>& out) const;

  // Extract with an external work buffer (avoids internal 33KB allocation).
  ZipError extract(IZipFile& file, const ZipEntry& entry, std::vector<uint8_t>& out, uint8_t* work_buf,
                   size_t work_buf_size) const;

  // Extract an entry via streaming callback. Uses a small fixed buffer.
  // work_buf must be at least 33KB for deflate (32KB dict + 1KB input).
  ZipError extract_streaming(IZipFile& file, const ZipEntry& entry, ZipDataCallback callback, void* user_data,
                             uint8_t* work_buf, size_t work_buf_size) const;

 private:
  std::vector<ZipEntry> entries_;
  std::vector<char> name_blob_;  // contiguous storage for all entry names
};

// IZipFile implementation backed by FILE* (for desktop and tests).
class StdioZipFile : public IZipFile {
 public:
  StdioZipFile() = default;
  ~StdioZipFile() override;

  bool open(const char* path);
  void close();
  bool is_open() const;

  bool seek(int64_t offset, int whence) override;
  int64_t tell() override;
  size_t read(void* buf, size_t size) override;

 private:
  void* fp_ = nullptr;  // FILE*, but we avoid including <cstdio> in the header
};

// Streaming ZIP entry reader implementing IXmlInput.
// Decompresses a ZIP entry on-demand with constant memory (~44KB for deflate).
// Usage:
//   ZipEntryInput input;
//   std::vector<uint8_t> work(ZipEntryInput::kMinWorkBufSize);
//   input.open(file, entry, work.data(), work.size());
//   XmlReader reader;
//   uint8_t xml_buf[4096];
//   reader.open(input, xml_buf, sizeof(xml_buf));
class ZipEntryInput : public IXmlInput {
 public:
  static constexpr size_t kDecompSize = 11264;  // >= sizeof(tinfl_decompressor), 256-aligned
  static constexpr size_t kDictSize = 32768;
  // Work buffer layout: [decompressor | dict(32KB) | input_buf(remaining)]
  static constexpr size_t kMinWorkBufSize = kDecompSize + kDictSize + 256;

  ZipEntryInput() = default;
  ~ZipEntryInput() override;

  ZipEntryInput(const ZipEntryInput&) = delete;
  ZipEntryInput& operator=(const ZipEntryInput&) = delete;

  // Prepare for streaming reads. For deflate entries, work_buf must be
  // >= kMinWorkBufSize bytes. For stored entries, work_buf is used as a
  // read-through buffer (any non-zero size works).
  // work_buf is borrowed and must outlive this object.
  ZipError open(IZipFile& file, const ZipEntry& entry, uint8_t* work_buf, size_t work_buf_size);

  size_t read(void* buf, size_t max_size) override;

  bool has_error() const {
    return error_;
  }

 private:
  IZipFile* file_ = nullptr;
  void* decomp_ = nullptr;  // tinfl_decompressor* (points into work_buf, NOT heap-allocated)

  uint8_t* dict_ = nullptr;
  uint8_t* in_buf_ = nullptr;
  size_t in_buf_capacity_ = 0;

  size_t in_buf_size_ = 0;
  size_t in_buf_ofs_ = 0;
  size_t in_remaining_ = 0;

  size_t dict_ofs_ = 0;
  size_t out_start_ = 0;
  size_t out_avail_ = 0;

  bool done_ = false;
  bool error_ = false;
  uint16_t compression_ = 0;

  // For stored entries: read-ahead via work_buf
  uint8_t* store_buf_ = nullptr;
  size_t store_buf_capacity_ = 0;
  size_t store_remaining_ = 0;  // bytes not yet read from SD
  size_t store_buf_avail_ = 0;  // bytes in store_buf_ ready to serve
  size_t store_buf_ofs_ = 0;    // read cursor into store_buf_
};

}  // namespace microreader
