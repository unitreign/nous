#include "BookIndex.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <queue>
#include <string>
#include <vector>

#include "../HeapLog.h"
#include "../display/DrawBuffer.h"
#include "Book.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#include "esp_log.h"
#define MR_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define MR_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define MR_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#include <filesystem>
namespace fs = std::filesystem;
#define MR_LOGI(tag, fmt, ...) (void)0
#define MR_LOGW(tag, fmt, ...) (void)0
#define MR_LOGE(tag, fmt, ...) (void)0
#endif

namespace microreader {

BookIndex& BookIndex::instance() {
  static BookIndex instance;
  return instance;
}

bool BookIndex::is_book_path(const char* path) {
  if (!path) return false;
  const char* slash = std::strrchr(path, '/');
  const char* name = slash ? slash + 1 : path;
  const size_t name_len = std::strlen(name);
  if (name_len <= 5) return false;
  const char* ext = name + name_len - 5;
  char ext_lower[5];
  for (int i = 0; i < 5; ++i)
    ext_lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
  return std::memcmp(ext_lower, ".epub", 5) == 0;
}

bool BookIndex::add_entry(std::string_view path, std::string_view title, std::string_view author,
                          uint32_t last_open_order) {
  if (static_cast<int>(entries_.size()) >= MAX_BOOKS)
    return false;
  BookIndexEntry entry;
  entry.path = pool_.add(path);
  entry.title = pool_.add(title);
  entry.author = pool_.add(author);
  entry.last_open_order = last_open_order;
  entries_.push_back(entry);
  return true;
}

bool BookIndex::load(const std::string& index_file) {
  FILE* f = std::fopen(index_file.c_str(), "rb");
  if (!f)
    return false;

  entries_.clear();
  pool_.reset();

  char line[1024];
  bool first_line = true;
  bool needs_rebuild = false;
  while (std::fgets(line, sizeof(line), f) && static_cast<int>(entries_.size()) < MAX_BOOKS) {
    if (first_line) {
      first_line = false;
      if (std::strncmp(line, "#microreader-index v", 20) == 0) {
        uint32_t v = static_cast<uint32_t>(std::strtoul(line + 20, nullptr, 10));
        if (v != INDEX_FORMAT_VERSION)
          needs_rebuild = true;
        continue;
      } else {
        needs_rebuild = true;
      }
    }

    // Remove newline
    size_t len = std::strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
      len--;
    }
    if (len > 0 && line[len - 1] == '\r') {
      line[len - 1] = '\0';
      len--;
    }

    // Format: path|title|author[|last_open_order]
    char* sep1 = std::strchr(line, '|');
    if (!sep1)
      continue;
    *sep1 = '\0';
    char* sep2 = std::strchr(sep1 + 1, '|');
    if (!sep2)
      continue;
    *sep2 = '\0';

    // Optional 4th field: last_open_order
    uint32_t order = 0;
    char* sep3 = std::strchr(sep2 + 1, '|');
    if (sep3) {
      *sep3 = '\0';
      order = static_cast<uint32_t>(std::strtoul(sep3 + 1, nullptr, 10));
    }
    add_entry(line, sep1 + 1, sep2 + 1, order);
  }

  std::fclose(f);
  if (needs_rebuild)
    return false;
  return true;
}

bool BookIndex::save(const std::string& index_file) const {
  FILE* f = std::fopen(index_file.c_str(), "wb");
  if (!f)
    return false;

  std::fprintf(f, "#microreader-index v%u\n", INDEX_FORMAT_VERSION);

  for (const auto& entry : entries_) {
    auto path_v = entry.path.view(pool_);
    auto title_v = entry.title.view(pool_);
    auto author_v = entry.author.view(pool_);
    std::fprintf(f, "%.*s|%.*s|%.*s|%u\n", static_cast<int>(path_v.size()), path_v.data(),
                 static_cast<int>(title_v.size()), title_v.data(), static_cast<int>(author_v.size()), author_v.data(),
                 static_cast<unsigned>(entry.last_open_order));
  }
  std::fclose(f);
  return true;
}

void BookIndex::clear_entries() {
  { std::vector<BookIndexEntry> tmp; entries_.swap(tmp); }
  pool_.reset();
  generation_ = 0;
}

void BookIndex::ensure_loaded_(const std::string& index_path) {
  if (!entries_.empty()) return;
  // entries_ is empty (e.g. MainMenu was stopped). Reload from disk so the
  // upcoming mutation merges with the existing on-disk state instead of
  // truncating the file. If the file doesn't exist, load() returns false and
  // entries_ stays empty — the caller's save() will then create it fresh.
  load(index_path);
}

bool BookIndex::index_file(const std::string& path, const std::string& index_path, DrawBuffer& buf) {
  Book book;
  if (book.open(path.c_str(), buf.scratch_buf1(), buf.scratch_buf2(), false) != EpubError::Ok) {
    MR_LOGW("index", "index_file: Book::open failed for '%s'", path.c_str());
    return false;
  }
  auto meta = book.metadata();
  const std::string author = meta.author.value_or("");
  ensure_loaded_(index_path);
  remove_entry(path);
  add_entry(path, meta.title, author);
  MR_LOGI("index", "index_file: added '%s' (entries=%zu)", path.c_str(), entries_.size());
  ++generation_;
  return save(index_path);
}

bool BookIndex::remove_path(const std::string& path, const std::string& index_path) {
  ensure_loaded_(index_path);
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const BookIndexEntry& e) { return e.path.view(pool_) == path; });
  if (it == entries_.end()) {
    MR_LOGW("index", "remove_path: '%s' not in index (entries=%zu)", path.c_str(), entries_.size());
    return true;  // not indexed — nothing to do, no save needed
  }
  MR_LOGI("index", "remove_path: removing '%s'", path.c_str());
  entries_.erase(it);
  ++generation_;
  bool ok = save(index_path);
  if (!ok) MR_LOGE("index", "remove_path: save FAILED for '%s'", path.c_str());
  return ok;
}

bool BookIndex::rename_in_place(const std::string& src, const std::string& dst, const std::string& index_path) {
  ensure_loaded_(index_path);
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const BookIndexEntry& e) { return e.path.view(pool_) == src; });
  if (it == entries_.end())
    return false;  // src not indexed — caller may fall back to index_file(dst)
  it->path = pool_.add(dst);
  ++generation_;
  return save(index_path);
}

void BookIndex::remove_entry(std::string_view path) {
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const BookIndexEntry& e) { return e.path.view(pool_) == path; });
  if (it != entries_.end())
    entries_.erase(it);
}

void BookIndex::set_last_opened(std::string_view path, uint32_t order) {
  for (auto& entry : entries_) {
    if (entry.path.view(pool_) == path) {
      entry.last_open_order = order;
      return;
    }
  }
}

void BookIndex::build_index(const std::string& root_dir, DrawBuffer& buf) {
  std::vector<std::pair<std::string, uint32_t>> old_orders;
  for (const auto& e : entries_)
    if (e.last_open_order > 0)
      old_orders.emplace_back(e.title.to_string(pool_) + '\x01' + e.author.to_string(pool_), e.last_open_order);

  entries_.clear();
  pool_.reset();
  buf.show_loading("Scanning...", 0);
  // Process epub files as we find them to avoid storing all paths in memory.
  int done = 0;
  int total = 0;

  // Reuse one Book instance across all iterations so the ZipReader's internal
  // vectors (entries_, name_blob_) retain their allocated capacity — repeated
  // alloc/free of same-sized buffers fragments the heap on ESP32.
  Book book;

  // Helper to process a single epub path (keeps peak memory low)
  auto process_path = [&](const std::string& path) {
    if (static_cast<int>(entries_.size()) >= MAX_BOOKS)
      return;
    buf.show_loading("Indexing...", total > 0 ? 10 + (done * 90 / total) : 10);
    book.close();
    if (book.open(path.c_str(), buf.scratch_buf1(), buf.scratch_buf2(), false) == EpubError::Ok) {
      auto meta = book.metadata();
      const std::string author = meta.author.value_or("");
      add_entry(path, meta.title, author);
      const std::string key = std::string(meta.title) + '\x01' + author;
      for (const auto& old : old_orders) {
        if (old.first == key) {
          entries_.back().last_open_order = old.second;
          break;
        }
      }
    }
    book.close();
    done++;
  };

  // Single iterator helper: calls `cb(path)` for each .epub found
  auto iterate_epubs = [&](const std::function<void(const std::string&)>& cb) {
    std::queue<std::string> q;
    q.push(root_dir);
    while (!q.empty()) {
      std::string current_dir = std::move(q.front());
      q.pop();

      // Called for each entry in current_dir; shared dot-skip and .epub check.
      auto visit = [&](const char* name, bool is_dir, const std::string& fullpath) {
        if (name[0] == '.') return;
        if (is_dir) { q.push(fullpath); return; }
        size_t len = std::strlen(name);
        if (len > 5) {
          const char* ext = name + len - 5;
          char ext_lower[6];
          for (int i = 0; i < 5; i++)
            ext_lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
          ext_lower[5] = '\0';
          if (std::strcmp(ext_lower, ".epub") == 0)
            cb(fullpath);
        }
      };

#ifdef ESP_PLATFORM
      DIR* dir = opendir(current_dir.c_str());
      if (!dir) continue;
      struct dirent* ent;
      while ((ent = readdir(dir)) != nullptr) {
        std::string fullpath = current_dir + "/" + ent->d_name;
        visit(ent->d_name, ent->d_type == DT_DIR, fullpath);
      }
      closedir(dir);
#else
      try {
        for (const auto& entry : fs::directory_iterator(current_dir, fs::directory_options::skip_permission_denied)) {
          std::string fullpath = entry.path().string();
          for (char& c : fullpath) if (c == '\\') c = '/';
          std::string fname = entry.path().filename().string();
          visit(fname.c_str(), entry.is_directory(), fullpath);
        }
      } catch (...) {}
#endif
    }
  };

  iterate_epubs([&](const std::string& p) { total++; });
  MR_LOGI("index", "Found %d epub(s), indexing...", total);
  iterate_epubs(process_path);
  MR_LOGI("index", "Indexed %d book(s)", done);

  // build_index is a structural mutation — bump so observers (MainMenu) refresh
  // even if the call doesn't go through index_file/remove_path/rename_in_place.
  ++generation_;

  if (done > 0)
    buf.show_loading("Indexing...", 100);
}

}  // namespace microreader
