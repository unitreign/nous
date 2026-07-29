#pragma once

#include <optional>
#include <string>

#include "ListMenuScreen.h"

namespace microreader {

class AlertScreen final : public ListMenuScreen {
 public:
  const char* name() const override { return "Alert"; }

  void set_message(const char* title, const char* body) {
    title_ = title;
    body_ = body;
  }

 protected:
  void on_start() override {}
  void on_select(int) override {}
  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;

 private:
  std::string title_;
  std::string body_;
};

}  // namespace microreader
