#pragma once

#include <string>
#include <vector>

#include "../display/DrawBuffer.h"
#include "ListMenuScreen.h"

namespace microreader {

// Shows books from the .hidden/ subfolder of the books directory.
// Accessed via long-press of the back button on the main book list.
class HiddenBooksMenu final : public ListMenuScreen {
 public:
  HiddenBooksMenu() = default;

  const char* name() const override { return "Hidden"; }

 protected:
  void on_start() override;
  void on_select(int index) override;
  void on_back() override;

 private:
  std::vector<std::string> paths_;
  void scan_();
};

}  // namespace microreader
