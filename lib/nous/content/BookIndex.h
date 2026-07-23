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
  uint64_t read_time_ms = 0;     // cumulative reading time in milliseconds
};

static constexpr int MAX_BOOKS = 250;
static constexpr uint32_t INDEX_FORMAT_VERSION = 2;

// Bumping this discards every stored read_time_ms while leaving the book list,
// titles, authors and open ordering intact. Epoch 1 clears the totals produced
// by the pre-fix accounting, which accumulated an already-cumulative value and
// so grew quadratically (see BookIndex::update_read_time).
//
// Deliberately separate from INDEX_FORMAT_VERSION: this is a data reset, not a
// layout change, and a version bump would instead force a full metadata-based
// rescan — which would carry the poisoned values straight back in via
// build_index().
static constexpr uint32_t INDEX_STATS_EPOCH = 1;

class BookIndex {
 public:
  static BookIndex& instance();

  // Returns true if path ends with .epub (case-insensitive) and has a non-empty stem.
  static bool is_book_path(const char* path);

  bool load(const std::string& index_file);
  bool save(const std::string& index_file) const;

  // Recursively scan root_dir for EPUBs and rebuild the index.
  void build_index(const std::string& root_dir, DrawBuffer& buf);

  const std::vector<BookIndexEntry>& entries() const { return entries_; }
  const StringPool& pool() const { return pool_; }

  // Bumped on every logical mutation (index_file / remove_path / rename_in_place /
  // build_index). MainMenu caches this to detect external changes. load() does not
  // bump; clear_entries() resets it to 0.
  uint64_t generation() const { return generation_; }

  // Returns false (no-op) if MAX_BOOKS has been reached.
  bool add_entry(std::string_view path, std::string_view title, std::string_view author,
                 uint32_t last_open_order = 0, uint64_t read_time_ms = 0);

  // Update read_time_ms for a book and save to disk. No-op if not found.
  void update_read_time(std::string_view path, uint64_t ms, const std::string& index_path);

  // Updates in-memory entry only; call save() to persist.
  void set_last_opened(std::string_view path, uint32_t order);

  // No-op if not found. Call save() to persist.
  void remove_entry(std::string_view path);

  // Open path as an EPUB, upsert its index entry, and save to index_path.
  // Reloads from disk first if entries_ is empty, to avoid truncating the .dat.
  bool index_file(const std::string& path, const std::string& index_path, DrawBuffer& buf);

  // Remove path's entry and save to index_path. No-op (returns true) if not indexed.
  // Reloads from disk first if entries_ is empty.
  bool remove_path(const std::string& path, const std::string& index_path);

  // Rename src→dst in-place, preserving metadata. Returns false if src not indexed
  // (caller may fall back to index_file). Reloads from disk first if entries_ is empty.
  bool rename_in_place(const std::string& src, const std::string& dst, const std::string& index_path);

  void clear_entries();

 private:
  std::vector<BookIndexEntry> entries_;
  StringPool pool_;
  uint64_t generation_ = 0;
  BookIndex() = default;

  // Loads from index_path if entries_ is empty, preventing .dat truncation
  // when a mutation arrives after MainMenu::stop() cleared in-memory state.
  void ensure_loaded_(const std::string& index_path);
};

}  // namespace microreader
