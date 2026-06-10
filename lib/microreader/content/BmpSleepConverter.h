#pragma once

namespace microreader {

// Convert a BMP file to MGR2 format (2bpp, kLutFactoryQuality encoding).
// Supports 8/24/32-bit uncompressed BMPs; scales to 800x480 with nearest-neighbor.
// Returns true on success; on failure any partial output file is removed.
bool convert_bmp_to_mgr2(const char* bmp_path, const char* mgr_out_path);

}  // namespace microreader
