#include "ZipReader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

// We only need the tinfl decompressor from miniz.
// Define MINIZ_NO_ZLIB_COMPATIBLE_NAMES to avoid polluting the namespace.
#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#include "miniz.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
static constexpr char kZipTag[] = "zip";
// Returns largest contiguous free block on ESP32; always passes on desktop.
static bool heap_can_alloc(size_t bytes) {
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  bool ok = largest >= bytes + 2048;  // 2 KB overhead margin
  if (!ok)
    ESP_LOGW(kZipTag, "heap preflight FAIL: need %u, largest=%u", (unsigned)bytes, (unsigned)largest);
  return ok;
}
#else
static inline bool heap_can_alloc(size_t) {
  return true;
}
#endif

namespace microreader {

static_assert(sizeof(tinfl_decompressor) <= ZipEntryInput::kDecompSize, "kDecompSize too small for tinfl_decompressor");

// ---------------------------------------------------------------------------
// ZIP format constants and on-disk structures
// ---------------------------------------------------------------------------

static constexpr uint32_t kEndCentralDirSig = 0x06054b50;
static constexpr uint32_t kCentralDirEntrySig = 0x02014b50;
static constexpr uint32_t kLocalHeaderSig = 0x04034b50;

#pragma pack(push, 1)
struct EndCentralDir {
  uint32_t signature;
  uint16_t disk_number;
  uint16_t central_dir_start_disk;
  uint16_t num_entries_this_disk;
  uint16_t total_entries;
  uint32_t central_dir_size;
  uint32_t central_dir_offset;
  uint16_t comment_length;
};

struct CentralDirEntry {
  uint32_t signature;
  uint16_t version_made;
  uint16_t version_needed;
  uint16_t flags;
  uint16_t compression;
  uint16_t mod_time;
  uint16_t mod_date;
  uint32_t crc32;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint16_t filename_len;
  uint16_t extra_len;
  uint16_t comment_len;
  uint16_t disk_start;
  uint16_t internal_attr;
  uint32_t external_attr;
  uint32_t local_header_offset;
};

struct LocalFileHeader {
  uint32_t signature;
  uint16_t version_needed;
  uint16_t flags;
  uint16_t compression;
  uint16_t mod_time;
  uint16_t mod_date;
  uint32_t crc32;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint16_t filename_len;
  uint16_t extra_len;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool read_exact(IZipFile& file, void* buf, size_t size) {
  size_t got = file.read(buf, size);
  return got == size;
}

static ZipError find_end_central_dir(IZipFile& file, EndCentralDir& eocd) {
  uint8_t buf[1024];

  // Seek to last 1024 bytes (or start of file)
  file.seek(0, SEEK_END);
  int64_t file_size = file.tell();
  if (file_size < static_cast<int64_t>(sizeof(EndCentralDir))) {
    return ZipError::InvalidData;
  }

  int64_t search_start = (file_size > 1024) ? (file_size - 1024) : 0;
  size_t search_len = static_cast<size_t>(file_size - search_start);
  file.seek(search_start, SEEK_SET);
  if (file.read(buf, search_len) != search_len) {
    return ZipError::ReadError;
  }

  // Search backwards for the end-of-central-directory signature
  for (int i = static_cast<int>(search_len) - static_cast<int>(sizeof(EndCentralDir)); i >= 0; --i) {
    uint32_t sig;
    memcpy(&sig, &buf[i], 4);
    if (sig == kEndCentralDirSig) {
      memcpy(&eocd, &buf[i], sizeof(EndCentralDir));
      return ZipError::Ok;
    }
  }
  return ZipError::InvalidSignature;
}

// ---------------------------------------------------------------------------
// ZipReader
// ---------------------------------------------------------------------------

ZipError ZipReader::open(IZipFile& file) {
  entries_.clear();
  name_blob_.clear();

  EndCentralDir eocd{};
  ZipError err = find_end_central_dir(file, eocd);
  if (err != ZipError::Ok)
    return err;

  if (eocd.total_entries == 0)
    return ZipError::InvalidData;

  entries_.reserve(eocd.total_entries);

  // Bulk-read the entire central directory into memory (one I/O operation)
  // then parse from the buffer.  This replaces the previous two-pass approach
  // that did ~2×N individual reads/seeks — catastrophically slow over SPI/SD.
  const uint32_t cd_size = eocd.central_dir_size;
  const int64_t cd_start = eocd.central_dir_offset;
  file.seek(cd_start, SEEK_SET);

#ifdef ESP_PLATFORM
  {
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(kZipTag, "open: entries=%u cd_size=%u free_largest=%u", (unsigned)eocd.total_entries, (unsigned)cd_size,
             (unsigned)largest);
  }
#endif

  if (!heap_can_alloc(cd_size))
    return ZipError::OutOfMemory;
  std::vector<uint8_t> cd_buf(cd_size);
  uint8_t* cd_ptr = cd_buf.data();
  if (file.read(cd_ptr, cd_size) != cd_size)
    return ZipError::ReadError;

  // First pass over in-memory buffer: count total name bytes.
  size_t total_name_bytes = 0;
  {
    size_t pos = 0;
    for (uint16_t i = 0; i < eocd.total_entries; ++i) {
      if (pos + sizeof(CentralDirEntry) > cd_size)
        return ZipError::InvalidData;
      CentralDirEntry cde;
      memcpy(&cde, cd_ptr + pos, sizeof(cde));
      if (cde.signature != kCentralDirEntrySig)
        return ZipError::InvalidSignature;
      total_name_bytes += cde.filename_len;
      pos += sizeof(cde) + cde.filename_len + cde.extra_len + cde.comment_len;
    }
  }

#ifdef ESP_PLATFORM
  ESP_LOGI(kZipTag, "open: name_blob=%u", (unsigned)total_name_bytes);
#endif
  if (!heap_can_alloc(total_name_bytes))
    return ZipError::OutOfMemory;
  name_blob_.resize(total_name_bytes);

  // Second pass: build entries, copy names into contiguous blob.
  size_t pos = 0;
  size_t blob_offset = 0;
  for (uint16_t i = 0; i < eocd.total_entries; ++i) {
    CentralDirEntry cde;
    memcpy(&cde, cd_ptr + pos, sizeof(cde));
    pos += sizeof(cde);

    // Copy filename into blob.
    memcpy(name_blob_.data() + blob_offset, cd_ptr + pos, cde.filename_len);
    pos += cde.filename_len + cde.extra_len + cde.comment_len;

    ZipEntry entry;
    entry.name = std::string_view(name_blob_.data() + blob_offset, cde.filename_len);
    entry.uncompressed_size = cde.uncompressed_size;
    entry.compressed_size = cde.compressed_size;
    entry.local_header_offset = cde.local_header_offset;
    entry.compression = cde.compression;
    entries_.push_back(entry);

    blob_offset += cde.filename_len;
  }

  return ZipError::Ok;
}

const ZipEntry* ZipReader::find(const char* name) const {
  for (const auto& e : entries_) {
    if (e.name == name)
      return &e;
  }
  return nullptr;
}

ZipError ZipReader::read_local_entry(IZipFile& file, uint32_t offset, ZipEntry& out) {
  file.seek(offset, SEEK_SET);

  LocalFileHeader lfh{};
  if (!read_exact(file, &lfh, sizeof(lfh)))
    return ZipError::ReadError;
  if (lfh.signature != kLocalHeaderSig)
    return ZipError::InvalidSignature;

  out.local_header_offset = offset;
  out.data_offset = offset + static_cast<uint32_t>(sizeof(LocalFileHeader)) + lfh.filename_len + lfh.extra_len;
  out.compression = lfh.compression;
  out.compressed_size = lfh.compressed_size;
  out.uncompressed_size = lfh.uncompressed_size;
  out.name = {};
  return ZipError::Ok;
}

// Seek to the start of the compressed data for an entry.
static ZipError seek_to_data(IZipFile& file, const ZipEntry& entry) {
  // Fast path: data_offset was pre-computed by read_local_entry — skip
  // re-reading the local header entirely (saves one seek + 30-byte read).
  if (entry.data_offset != 0) {
    file.seek(entry.data_offset, SEEK_SET);
    return ZipError::Ok;
  }

  file.seek(entry.local_header_offset, SEEK_SET);

  LocalFileHeader lfh{};
  if (!read_exact(file, &lfh, sizeof(lfh)))
    return ZipError::ReadError;
  if (lfh.signature != kLocalHeaderSig)
    return ZipError::InvalidSignature;

  // Skip filename + extra field to reach the data
  int64_t skip = static_cast<int64_t>(lfh.filename_len) + lfh.extra_len;
  file.seek(skip, SEEK_CUR);

  return ZipError::Ok;
}

ZipError ZipReader::extract(IZipFile& file, const ZipEntry& entry, std::vector<uint8_t>& out) const {
  // Allocate a work buffer internally: decomp + dict + input.
  static constexpr size_t kWorkBufSize = ZipEntryInput::kDecompSize + TINFL_LZ_DICT_SIZE + 1024;
  std::vector<uint8_t> work_buf(kWorkBufSize);
  return extract(file, entry, out, work_buf.data(), work_buf.size());
}

ZipError ZipReader::extract(IZipFile& file, const ZipEntry& entry, std::vector<uint8_t>& out, uint8_t* work_buf,
                            size_t work_buf_size) const {
  out.resize(entry.uncompressed_size);
  if (entry.uncompressed_size == 0)
    return ZipError::Ok;

  auto cb = [](const uint8_t* data, size_t size, void* user_data) -> bool {
    auto* ctx = static_cast<std::pair<std::vector<uint8_t>*, size_t>*>(user_data);
    if (ctx->second + size > ctx->first->size())
      return false;
    memcpy(ctx->first->data() + ctx->second, data, size);
    ctx->second += size;
    return true;
  };

  std::pair<std::vector<uint8_t>*, size_t> ctx{&out, 0};
  return extract_streaming(file, entry, cb, &ctx, work_buf, work_buf_size);
}

ZipError ZipReader::extract_streaming(IZipFile& file, const ZipEntry& entry, ZipDataCallback callback, void* user_data,
                                      uint8_t* work_buf, size_t work_buf_size) const {
  ZipError err = seek_to_data(file, entry);
  if (err != ZipError::Ok)
    return err;

  if (entry.compression == 0) {
    // Stored: read directly, use work_buf as copy buffer
    size_t chunk = std::min(work_buf_size, static_cast<size_t>(4096));
    size_t remaining = entry.uncompressed_size;
    while (remaining > 0) {
      size_t to_read = std::min(remaining, chunk);
      size_t got = file.read(work_buf, to_read);
      if (got == 0)
        return ZipError::ReadError;
      if (!callback(work_buf, got, user_data))
        return ZipError::Ok;  // user abort
      remaining -= got;
    }
    return ZipError::Ok;
  }

  if (entry.compression != 8) {
    return ZipError::UnsupportedCompression;
  }

  // Deflate: use tinfl with dictionary/wrapping mode (raw deflate, no zlib header).
  // Layout of work_buf: [tinfl_decompressor | dict(32KB) | input_buf(remaining)]
  static constexpr size_t kDecompSize = ZipEntryInput::kDecompSize;
  size_t min_size = kDecompSize + TINFL_LZ_DICT_SIZE + 256;
  if (work_buf_size < min_size)
    return ZipError::DecompressionFailed;

  auto* decomp = reinterpret_cast<tinfl_decompressor*>(work_buf);
  uint8_t* dict = work_buf + kDecompSize;
  uint8_t* in_buf = dict + TINFL_LZ_DICT_SIZE;
  size_t in_buf_capacity = work_buf_size - kDecompSize - TINFL_LZ_DICT_SIZE;

  tinfl_init(decomp);

  size_t in_remaining = entry.compressed_size;
  size_t in_buf_size = 0;
  size_t in_buf_ofs = 0;
  size_t dict_ofs = 0;

  for (;;) {
    // Refill input buffer if needed
    if (in_buf_ofs >= in_buf_size && in_remaining > 0) {
      size_t to_read = std::min(in_remaining, in_buf_capacity);
      in_buf_size = file.read(in_buf, to_read);
      if (in_buf_size == 0)
        return ZipError::ReadError;
      in_remaining -= in_buf_size;
      in_buf_ofs = 0;
    }

    size_t in_bytes = in_buf_size - in_buf_ofs;
    size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;

    mz_uint32 flags = 0;
    if (in_remaining > 0)
      flags |= TINFL_FLAG_HAS_MORE_INPUT;

    tinfl_status status =
        tinfl_decompress(decomp, in_buf + in_buf_ofs, &in_bytes, dict, dict + dict_ofs, &out_bytes, flags);

    in_buf_ofs += in_bytes;

    if (out_bytes > 0) {
      if (!callback(dict + dict_ofs, out_bytes, user_data)) {
        return ZipError::Ok;  // user abort
      }
      dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
    }

    if (status == TINFL_STATUS_DONE)
      break;
    if (status < TINFL_STATUS_DONE)
      return ZipError::DecompressionFailed;
  }

  return ZipError::Ok;
}

// ---------------------------------------------------------------------------
// StdioZipFile
// ---------------------------------------------------------------------------

StdioZipFile::~StdioZipFile() {
  close();
}

bool StdioZipFile::open(const char* path) {
  close();
  fp_ = std::fopen(path, "rb");
  return fp_ != nullptr;
}

void StdioZipFile::close() {
  if (fp_) {
    std::fclose(static_cast<FILE*>(fp_));
    fp_ = nullptr;
  }
}

bool StdioZipFile::is_open() const {
  return fp_ != nullptr;
}

bool StdioZipFile::seek(int64_t offset, int whence) {
  if (!fp_)
    return false;
#ifdef _WIN32
  return _fseeki64(static_cast<FILE*>(fp_), offset, whence) == 0;
#else
  return fseek(static_cast<FILE*>(fp_), static_cast<long>(offset), whence) == 0;
#endif
}

int64_t StdioZipFile::tell() {
  if (!fp_)
    return -1;
#ifdef _WIN32
  return _ftelli64(static_cast<FILE*>(fp_));
#else
  return ftell(static_cast<FILE*>(fp_));
#endif
}

size_t StdioZipFile::read(void* buf, size_t size) {
  if (!fp_)
    return 0;
  return std::fread(buf, 1, size, static_cast<FILE*>(fp_));
}

// ---------------------------------------------------------------------------
// ZipEntryInput — streaming IXmlInput backed by a ZIP entry
// ---------------------------------------------------------------------------

ZipEntryInput::~ZipEntryInput() {
  // decomp_ points into the borrowed work_buf — nothing to delete.
}

ZipError ZipEntryInput::open(IZipFile& file, const ZipEntry& entry, uint8_t* work_buf, size_t work_buf_size) {
  file_ = &file;
  compression_ = entry.compression;
  done_ = false;
  error_ = false;
  out_avail_ = 0;

  ZipError err = seek_to_data(file, entry);
  if (err != ZipError::Ok)
    return err;

  if (compression_ == 0) {
    // Stored: use work_buf as a read-ahead buffer.
    // ZipEntryInput::read() will fill it in large chunks from SD,
    // then drain it without further SD calls.
    store_buf_ = work_buf;
    store_buf_capacity_ = work_buf_size;
    store_remaining_ = entry.uncompressed_size;
    store_buf_avail_ = 0;
    store_buf_ofs_ = 0;
    return ZipError::Ok;
  }

  if (compression_ != 8)
    return ZipError::UnsupportedCompression;

  if (work_buf_size < kMinWorkBufSize)
    return ZipError::DecompressionFailed;

  // Layout: [tinfl_decompressor | dict(32KB) | input_buf(remaining)]
  auto* d = reinterpret_cast<tinfl_decompressor*>(work_buf);
  tinfl_init(d);
  decomp_ = d;

  dict_ = work_buf + kDecompSize;
  in_buf_ = dict_ + kDictSize;
  in_buf_capacity_ = work_buf_size - kDecompSize - kDictSize;
  in_buf_size_ = 0;
  in_buf_ofs_ = 0;
  in_remaining_ = entry.compressed_size;
  dict_ofs_ = 0;
  out_start_ = 0;
  out_avail_ = 0;

  return ZipError::Ok;
}

size_t ZipEntryInput::read(void* buf, size_t max_size) {
  if (error_)
    return 0;

  auto* out = static_cast<uint8_t*>(buf);
  size_t total = 0;

  if (compression_ == 0) {
    // Stored: read-ahead buffering — refill store_buf_ in large chunks
    // (up to store_buf_capacity_) and drain from it, minimising SD read calls.
    while (total < max_size) {
      // Drain already-buffered data first.
      if (store_buf_avail_ > 0) {
        size_t n = std::min(max_size - total, store_buf_avail_);
        std::memcpy(out + total, store_buf_ + store_buf_ofs_, n);
        store_buf_ofs_ += n;
        store_buf_avail_ -= n;
        total += n;
        continue;
      }
      // Nothing buffered: read a large chunk from SD.
      if (store_remaining_ == 0)
        break;
      size_t to_read = std::min(store_remaining_, store_buf_capacity_);
      size_t got = file_->read(store_buf_, to_read);
      if (got == 0) {
        error_ = true;
        break;
      }
      store_remaining_ -= got;
      store_buf_ofs_ = 0;
      store_buf_avail_ = got;
    }
    if (store_buf_avail_ == 0 && store_remaining_ == 0)
      done_ = true;
    return total;
  }

  // Deflate: incremental decompression
  auto* decomp = static_cast<tinfl_decompressor*>(decomp_);

  while (total < max_size) {
    // 1. Drain pending output from dict
    if (out_avail_ > 0) {
      size_t n = std::min(out_avail_, max_size - total);
      std::memcpy(out + total, dict_ + out_start_, n);
      out_start_ += n;
      out_avail_ -= n;
      total += n;
      continue;
    }

    if (done_)
      break;

    // 2. Refill compressed input buffer if needed
    if (in_buf_ofs_ >= in_buf_size_ && in_remaining_ > 0) {
      size_t to_read = std::min(in_remaining_, in_buf_capacity_);
      in_buf_size_ = file_->read(in_buf_, to_read);
      if (in_buf_size_ == 0) {
        error_ = true;
        break;
      }
      in_remaining_ -= in_buf_size_;
      in_buf_ofs_ = 0;
    }

    // 3. Decompress one chunk
    size_t in_bytes = in_buf_size_ - in_buf_ofs_;
    size_t out_bytes = kDictSize - dict_ofs_;

    mz_uint32 flags = 0;
    if (in_remaining_ > 0)
      flags |= TINFL_FLAG_HAS_MORE_INPUT;

    tinfl_status status =
        tinfl_decompress(decomp, in_buf_ + in_buf_ofs_, &in_bytes, dict_, dict_ + dict_ofs_, &out_bytes, flags);

    in_buf_ofs_ += in_bytes;

    if (out_bytes > 0) {
      out_start_ = dict_ofs_;
      out_avail_ = out_bytes;
      dict_ofs_ = (dict_ofs_ + out_bytes) & (kDictSize - 1);
    }

    if (status == TINFL_STATUS_DONE) {
      done_ = true;
    } else if (status < TINFL_STATUS_DONE) {
      error_ = true;
      break;
    } else if (out_bytes == 0 && in_bytes == 0) {
      // tinfl made no progress — avoid infinite loop
      error_ = true;
      break;
    }
  }

  return total;
}

}  // namespace microreader
