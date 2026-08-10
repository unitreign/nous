#include "WhatsNewScreen.h"

#include "../display/DrawBuffer.h"

namespace microreader {

void WhatsNewScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  ListMenuScreen::update(buttons, buf, runtime);
}

void WhatsNewScreen::draw_all_(DrawBuffer& buf, std::optional<uint8_t>) const {
  buf.fill(true);
}

}  // namespace microreader
