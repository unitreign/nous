#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "StringPool.h"

namespace microreader {

class DrawBuffer;

struct BookIndexEntry {
  StringRef path{};
  StringRef title{};
  StringRef author{};
  uint32_t last_open_order = 0;  // 0 = never opened; higher = more recently opened
};

static constexpr int MAX_BOOKS = 250;

class BookIndex {
 public:
  static BookIndex& instance();

  // Returns true if `path` is under /sdcard/ and has a .epub extension
  // (case-insensitive). Mirrors the filter used by build_index so that
  // serial-side decisions about whether to update the index stay consistent
  // with what a full rebuild would consider a "book".
  static bool is_book_path(const char* path);

  bool load(const std::string& index_file);
  bool save(const std::string& index_file) const;

  // Scan a directory recursively, updating the index
  // Uses DrawBuffer for scratch buffers and for showing loading progress.
  void build_index(const std::string& root_dir, DrawBuffer& buf);

  const std::vector<BookIndexEntry>& entries() const {
    return entries_;
  }

  const StringPool& pool() const {
    return pool_;
  }

  // Monotonically increasing counter bumped on every mutation that changes the
  // logical contents of the index (index_file / remove_path / rename_in_place
  // / build_index). Observers (e.g. MainMenu) compare a cached value to detect
  // that the index changed since they last rendered. load() and clear_entries()
  // do NOT bump — they are state setup, not logical mutations.
  uint64_t generation() const { return generation_; }

  // Add an entry; uses the pool to store strings. Returns false (and is a
  // no-op) if MAX_BOOKS has been reached.
  bool add_entry(std::string_view path, std::string_view title, std::string_view author, uint32_t last_open_order = 0);

  // Record that a book was opened. `order` should be a monotonically
  // increasing counter (higher = more recently opened). Updates the in-memory
  // entry only; call save() afterwards to persist.
  void set_last_opened(std::string_view path, uint32_t order);

  // Remove the entry for `path`. No-op if not found. Call save() to persist.
  void remove_entry(std::string_view path);

  // Open `path` as an EPUB, extract its metadata, add/replace the index entry,
  // and persist to `index_path`. Loads the existing .dat first (ensure_loaded_)
  // so the on-disk index is never truncated when entries_ happens to be empty
  // in memory (e.g. when called while MainMenu is stopped). Bumps generation.
  bool index_file(const std::string& path, const std::string& index_path, DrawBuffer& buf);

  // Remove the entry for `path` and persist to `index_path`. Safe to call when
  // entries_ is empty in memory — reloads from disk first. No-op (returns true,
  // no save) if `path` isn't indexed. Bumps generation only when something
  // actually changed.
  bool remove_path(const std::string& path, const std::string& index_path);

  // Update the path of an existing entry from `src` to `dst`, preserving
  // title/author/last_open_order. Safe to call when entries_ is empty —
  // reloads from disk first. Returns false (no save) if `src` isn't indexed;
  // caller may then fall back to index_file(dst) to add it fresh. Bumps
  // generation only on success.
  bool rename_in_place(const std::string& src, const std::string& dst, const std::string& index_path);

  void clear_entries();

 private:
  std::vector<BookIndexEntry> entries_;
  StringPool pool_;
  uint64_t generation_ = 0;
  BookIndex() = default;

  // If entries_ is empty, attempt to load from `index_path`. No-op if entries_
  // is already populated or if the file doesn't exist. This is the key helper
  // that prevents the on-disk index from being truncated when a mutation
  // arrives while the in-memory cache has been cleared (e.g. MainMenu::stop).
  void ensure_loaded_(const std::string& index_path);
};

}  // namespace microreader
