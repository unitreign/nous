#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace microreader {

class DrawBuffer;

struct BookIndexEntry {
  std::string path;
  std::string title;
  std::string author;
  uint32_t last_open_order = 0;  // 0 = never opened; higher = more recently opened
};

class BookIndex {
 public:
  static BookIndex& instance();

  bool load(const std::string& index_file);
  bool save(const std::string& index_file) const;

  // Scan a directory recursively, updating the index
  // Uses DrawBuffer for scratch buffers and for showing loading progress.
  void build_index(const std::string& root_dir, DrawBuffer& buf);

  const std::vector<BookIndexEntry>& entries() const {
    return entries_;
  }

  // Record that a book was opened. `order` should be a monotonically
  // increasing counter (higher = more recently opened). Updates the in-memory
  // entry only; call save() afterwards to persist.
  void set_last_opened(const std::string& path, uint32_t order);

  void clear_entries() {
    entries_.clear();
  }

 private:
  std::vector<BookIndexEntry> entries_;
  BookIndex() = default;
};

}  // namespace microreader
