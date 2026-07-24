#pragma once

#include <optional>

#include "../Input.h"
#include "ListMenuScreen.h"

namespace microreader {

class WhatsNewScreen final : public ListMenuScreen {
 public:
  const char* name() const override { return "What's New"; }
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 protected:
  void on_start() override { page_ = 0; }
  void on_select(int index) override {}
  void draw_all_(DrawBuffer& buf, std::optional<uint8_t> battery_pct = std::nullopt) const override;

 private:
  int page_ = 0;  // 0 = current version, 1 = previous version
};

}  // namespace microreader
