#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace microreader {

// Compact pooled string storage for many small strings.
// Stores bytes in a single contiguous blob; returns StringRef offsets.
// This header is intentionally header-only and minimal to allow use in hot paths.

struct StringRef {
  uint32_t off = 0;
  uint16_t len = 0;

  std::string_view view(const class StringPool& pool) const;
  std::string to_string(const class StringPool& pool) const;
};

class StringPool {
 public:
  StringRef add(std::string_view s) {
    StringRef r;
    r.off = static_cast<uint32_t>(blob_.size());
    r.len = static_cast<uint16_t>(s.size());
    blob_.append(s.data(), s.size());
    return r;
  }

  std::string_view get(const StringRef& ref) const {
    if (ref.len == 0)
      return {};
    return {blob_.data() + ref.off, ref.len};
  }

  void reserve(size_t n) {
    blob_.reserve(n);
  }

 private:
  std::string blob_;
};

// Define StringRef helpers after StringPool is complete so we can call pool.get().
inline std::string_view StringRef::view(const StringPool& pool) const {
  if (len == 0)
    return {};
  return pool.get(*this);
}

inline std::string StringRef::to_string(const StringPool& pool) const {
  return std::string(view(pool));
}

}  // namespace microreader
