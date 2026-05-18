#pragma once
// FontLoader.h — shared helpers for loading font files in tests.
// Used by BitmapFontTest and HtmlExportTest.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "TestPaths.h"
#include "microreader/content/BitmapFont.h"

namespace fs = std::filesystem;

// Load a file into a byte vector. Returns empty on failure.
static inline std::vector<uint8_t> load_file_bytes(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.good())
    return {};
  f.seekg(0, std::ios::end);
  const auto size = static_cast<size_t>(f.tellg());
  if (size == 0)
    return {};
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(size);
  f.read(reinterpret_cast<char*>(data.data()), size);
  return f.good() ? data : std::vector<uint8_t>{};
}

// Extract the nth font entry from an FNTS bundle, returned as a standalone
// byte buffer that can be passed directly to BitmapFont::init().
// Returns empty vector if the index is out of range or the data is invalid.
static inline std::vector<uint8_t> extract_font_from_bundle(const std::vector<uint8_t>& bundle, size_t idx) {
  if (bundle.size() < 40)
    return {};
  const uint8_t* d = bundle.data();
  const size_t sz = bundle.size();
  if (d[0] != 'F' || d[1] != 'N' || d[2] != 'T' || d[3] != 'S' || d[5] < 1)
    return {};

  uint8_t num = d[4];
  if (idx >= num)
    return {};

  constexpr size_t kSizeTableOff = 8 + 32;
  uint32_t sizes[microreader::kMaxFontSizes] = {};
  for (int i = 0; i < num; i++) {
    const uint8_t* p = d + kSizeTableOff + i * 4;
    sizes[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
  }

  size_t off = kSizeTableOff + static_cast<size_t>(num) * 4;
  for (size_t i = 0; i < idx; i++)
    off += sizes[i];

  if (off + sizes[idx] > sz)
    return {};

  return std::vector<uint8_t>(d + off, d + off + sizes[idx]);
}

// Load one font by index suffix ("0", "1", ...) from the bookerly.mfb bundle.
// This replaces the old individual font-N.mbf files.
static inline std::vector<uint8_t> load_mbf(const std::string& suffix) {
  fs::path bundle_path = fs::path(repo_root()) / "sd" / "fonts" / "bookerly.mfb";
  if (!fs::exists(bundle_path))
    bundle_path = fs::path(repo_root()) / "resources" / "fonts" / "bookerly.mfb";
  auto bundle = load_file_bytes(bundle_path);
  if (bundle.empty())
    return {};
  size_t idx = static_cast<size_t>(std::stoi(suffix));
  return extract_font_from_bundle(bundle, idx);
}

// Load all font sizes from the bookerly.mfb FNTS bundle into font_set.
// font_data[0] holds the raw bundle bytes (must outlive font_set).
// prop_fonts must be pre-sized to at least kMaxFontSizes.
// Returns true on success.
static inline bool load_desktop_fonts(microreader::BitmapFontSet& font_set,
                                      std::vector<microreader::BitmapFont>& prop_fonts,
                                      std::vector<std::vector<uint8_t>>& font_data) {
  // Prefer sd/fonts/ (matches runtime), fall back to resources/fonts/ for CI.
  fs::path bundle_path = fs::path(repo_root()) / "sd" / "fonts" / "bookerly.mfb";
  if (!fs::exists(bundle_path))
    bundle_path = fs::path(repo_root()) / "resources" / "fonts" / "bookerly.mfb";

  font_data.clear();
  font_data.resize(1);
  font_data[0] = load_file_bytes(bundle_path);
  const auto& bundle = font_data[0];
  if (bundle.size() < 40)
    return false;

  const uint8_t* d = bundle.data();
  const size_t sz = bundle.size();

  // FNTS header: [FNTS:4][num:1][version:1][reserved:2][font_name:32][num×size:4][data...]
  if (d[0] != 'F' || d[1] != 'N' || d[2] != 'T' || d[3] != 'S' || d[5] < 1)
    return false;

  uint8_t num = d[4];
  if (num > microreader::kMaxFontSizes)
    num = microreader::kMaxFontSizes;

  constexpr size_t kSizeTableOff = 8 + 32;
  uint32_t sizes[microreader::kMaxFontSizes] = {};
  for (int i = 0; i < num; i++) {
    const uint8_t* p = d + kSizeTableOff + i * 4;
    sizes[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
  }

  prop_fonts.clear();
  prop_fonts.resize(microreader::kMaxFontSizes);
  font_set = microreader::BitmapFontSet();

  size_t off = kSizeTableOff + static_cast<size_t>(num) * 4;
  for (int i = 0; i < num; i++) {
    if (off + sizes[i] > sz)
      break;
    prop_fonts[i].init(d + off, sizes[i]);
    if (!prop_fonts[i].valid())
      return false;
    font_set.add(&prop_fonts[i]);
    off += sizes[i];
  }

  return font_set.valid();
}
