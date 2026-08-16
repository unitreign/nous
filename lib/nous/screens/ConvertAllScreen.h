#pragma once

#include <string>
#include <vector>

#include "ListMenuScreen.h"
#include "PickerOverlay.h"

namespace microreader {

// Interactive per-book convert/delete screen backed by ListMenuScreen.
// Row 0 = "Convert All" action. Rows 1..n = book entries (title + status subtitle).
// Selecting an unconverted book converts it; selecting a converted book deletes its cache.
class ConvertAllScreen final : public ListMenuScreen {
 public:
  ConvertAllScreen() = default;

  const char* name() const override { return "ConvertBooks"; }

  void stop() override;
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;
  std::string_view get_item_subtitle(int index) const override;
  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override;

 private:
  struct Entry {
    std::string path;
    std::string title;
    bool converted = false;
    bool has_cover = false;
  };

  std::vector<Entry>  entries_;
  mutable std::string subtitle_buf_;
  PickerOverlay       confirm_picker_;
  DrawBuffer*         cur_buf_     = nullptr;
  IRuntime*           cur_runtime_ = nullptr;

  static std::string derive_stem_(const std::string& path);
  void scan_entries_();
  void rebuild_items_();
  int  unconverted_count_() const;
  void do_convert_path_(const std::string& path, const std::string& title,
                        DrawBuffer& buf, IRuntime& runtime);
  void do_delete_(int idx, DrawBuffer& buf, IRuntime& runtime);
};

}  // namespace microreader
