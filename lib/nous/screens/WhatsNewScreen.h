#pragma once

#include <optional>

#include "ListMenuScreen.h"

namespace microreader {

class WhatsNewScreen final : public ListMenuScreen {
 public:
  const char* name() const override { return "What's New"; }

 protected:
  void on_start() override {}
  void on_select(int index) override {}
  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;
};

}  // namespace microreader
