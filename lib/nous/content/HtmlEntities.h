#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace microreader {

struct DecodedEntity {
  enum class Kind : uint8_t { Unknown = 0, Space, Utf8, Codepoint };

  Kind kind = Kind::Unknown;
  std::string_view utf8;
  uint32_t codepoint = 0;
};

DecodedEntity decode_html_entity(const char* entity, size_t entity_len);
void append_utf8(std::string& out, uint32_t code);

}  // namespace microreader
