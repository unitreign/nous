#include "HiddenBooksMenu.h"

#include <cctype>
#include <cstring>
#include <queue>
#include <string>

#include "../Application.h"

#ifdef ESP_PLATFORM
#include <dirent.h>
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

namespace microreader {

static bool is_epub_name(const char* name) {
  size_t len = std::strlen(name);
  if (len <= 5) return false;
  const char* ext = name + len - 5;
  char low[5];
  for (int i = 0; i < 5; ++i)
    low[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
  return low[0] == '.' && low[1] == 'e' && low[2] == 'p' && low[3] == 'u' && low[4] == 'b';
}

static std::string path_stem(const std::string& path) {
  const char* p = path.c_str();
  const char* slash = std::strrchr(p, '/');
  const char* name = slash ? slash + 1 : p;
  const char* dot = std::strrchr(name, '.');
  return dot ? std::string(name, static_cast<size_t>(dot - name)) : std::string(name);
}

void HiddenBooksMenu::scan_() {
  if (!app_ || !app_->main_menu() || !app_->main_menu()->books_dir())
    return;
  std::string hidden_dir = std::string(app_->main_menu()->books_dir()) + "/.hidden";

  std::queue<std::string> q;
  q.push(hidden_dir);
  while (!q.empty()) {
    std::string dir = std::move(q.front());
    q.pop();

#ifdef ESP_PLATFORM
    DIR* d = opendir(dir.c_str());
    if (!d) continue;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
      if (ent->d_name[0] == '.') continue;
      std::string full = dir + "/" + ent->d_name;
      if (ent->d_type == DT_DIR) {
        q.push(full);
      } else if (is_epub_name(ent->d_name)) {
        paths_.push_back(full);
      }
    }
    closedir(d);
#else
    try {
      for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
        if (e.is_directory()) {
          q.push(e.path().string());
          continue;
        }
        std::string fname = e.path().filename().string();
        if (is_epub_name(fname.c_str())) {
          std::string p = e.path().string();
          for (char& c : p) if (c == '\\') c = '/';
          paths_.push_back(p);
        }
      }
    } catch (...) {}
#endif
  }
}

void HiddenBooksMenu::on_start() {
  title_ = "Hidden";
  paths_.clear();
  scan_();
  if (paths_.empty()) {
    add_item("(empty)");
  } else {
    for (const auto& p : paths_)
      add_item(path_stem(p));
  }
}

void HiddenBooksMenu::on_select(int index) {
  if (paths_.empty() || index < 0 || index >= static_cast<int>(paths_.size()))
    return;
  app_->reader()->set_path(paths_[index]);
  app_->push_screen(ScreenId::Reader);
}

void HiddenBooksMenu::on_back() {
  app_->pop_screen();
}

}  // namespace microreader
