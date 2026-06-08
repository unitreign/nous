#include "BookIndex.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <queue>
#include <string>
#include <vector>

#include "../display/DrawBuffer.h"
#include "Book.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

namespace microreader {

BookIndex& BookIndex::instance() {
  static BookIndex instance;
  return instance;
}

bool BookIndex::load(const std::string& index_file) {
  FILE* f = std::fopen(index_file.c_str(), "rb");
  if (!f)
    return false;

  entries_.clear();
  char line[1024];
  while (std::fgets(line, sizeof(line), f)) {
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

    BookIndexEntry entry;
    entry.path = line;
    entry.title = sep1 + 1;

    // Optional 4th field: last_open_order
    char* sep3 = std::strchr(sep2 + 1, '|');
    if (sep3) {
      *sep3 = '\0';
      entry.author = sep2 + 1;
      entry.last_open_order = static_cast<uint32_t>(std::strtoul(sep3 + 1, nullptr, 10));
    } else {
      entry.author = sep2 + 1;
    }

    if (!entry.title.empty()) {
      entry.label = entry.title;
      if (!entry.author.empty()) {
        entry.label += " - ";
        entry.label += entry.author;
      }
    } else {
      // Fallback label to filename
      const char* name = entry.path.c_str();
      const char* sep = std::strrchr(name, '/');
#ifdef _WIN32
      const char* bsep = std::strrchr(name, '\\');
      if (bsep && (!sep || bsep > sep))
        sep = bsep;
#endif
      if (sep)
        name = sep + 1;
      const char* dot = std::strrchr(name, '.');
      size_t name_len = dot ? static_cast<size_t>(dot - name) : std::strlen(name);
      entry.label = std::string(name, name_len);
    }
    entries_.push_back(std::move(entry));
  }
  std::fclose(f);
  return true;
}

bool BookIndex::save(const std::string& index_file) const {
  FILE* f = std::fopen(index_file.c_str(), "wb");
  if (!f)
    return false;

  for (const auto& entry : entries_) {
    std::fprintf(f, "%s|%s|%s|%u\n", entry.path.c_str(), entry.title.c_str(), entry.author.c_str(),
                 static_cast<unsigned>(entry.last_open_order));
  }
  std::fclose(f);
  return true;
}

void BookIndex::set_last_opened(const std::string& path, uint32_t order) {
  for (auto& entry : entries_) {
    if (entry.path == path) {
      entry.last_open_order = order;
      return;
    }
  }
}

void BookIndex::build_index(const std::string& root_dir, DrawBuffer& buf) {
  entries_.clear();
  buf.show_loading("Scanning...", 0);
  // Process epub files as we find them to avoid storing all paths in memory.
  int done = 0;
  int total = 0;

  // Helper to process a single epub path (keeps peak memory low)
  auto process_path = [&](const std::string& path) {
    buf.show_loading("Indexing...", total > 0 ? 10 + (done * 90 / total) : 10);
    Book book;
    if (book.open(path.c_str(), buf.scratch_buf1(), buf.scratch_buf2(), false) == EpubError::Ok) {
      BookIndexEntry entry;
      entry.path = path;
      auto meta = book.metadata();
      entry.title = meta.title;
      if (meta.author) {
        entry.author = *meta.author;
      }

      if (!entry.title.empty()) {
        entry.label = entry.title;
        if (!entry.author.empty()) {
          entry.label += " - ";
          entry.label += entry.author;
        }
      } else {
        const char* name = path.c_str();
        const char* sep = std::strrchr(path.c_str(), '/');
        if (sep)
          name = sep + 1;
        const char* dot = std::strrchr(name, '.');
        size_t name_len = dot ? static_cast<size_t>(dot - name) : std::strlen(name);
        entry.label = std::string(name, name_len);
      }
      entries_.push_back(std::move(entry));
    }
    book.close();
    done++;
  };

  // Single iterator helper: calls `cb(path)` for each .epub found
  auto iterate_epubs = [&](const std::function<void(const std::string&)>& cb) {
#ifdef ESP_PLATFORM
    std::queue<std::string> q;
    q.push(root_dir);
    while (!q.empty()) {
      std::string current_dir = q.front();
      q.pop();
      DIR* dir = opendir(current_dir.c_str());
      if (!dir)
        continue;
      struct dirent* ent;
      while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.')
          continue;
        std::string fullpath = current_dir + "/" + ent->d_name;
        if (ent->d_type == DT_DIR) {
          q.push(fullpath);
        } else {
          size_t len = std::strlen(ent->d_name);
          if (len > 5) {
            const char* ext = ent->d_name + len - 5;
            char ext_lower[6];
            for (int i = 0; i < 5; i++)
              ext_lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
            ext_lower[5] = '\0';
            if (std::strcmp(ext_lower, ".epub") == 0)
              cb(fullpath);
          }
        }
      }
      closedir(dir);
    }
#else
    try {
      for (const auto& entry : fs::recursive_directory_iterator(root_dir)) {
        if (!entry.is_regular_file())
          continue;
        auto ext = entry.path().extension().string();
        for (char& c : ext)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".epub") {
          std::string path_str = entry.path().string();
          for (char& c : path_str)
            if (c == '\\')
              c = '/';
          cb(path_str);
        }
      }
    } catch (...) {}
#endif
  };

  // Count then process using the iterator
  iterate_epubs([&](const std::string& p) { total++; });
  iterate_epubs(process_path);

  // Refresh loading to 100% just before exit
  if (done > 0) {
    buf.show_loading("Indexing...", 100);
  }
}

}  // namespace microreader
