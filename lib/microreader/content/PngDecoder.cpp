// PngDecoder.cpp
//
// Minimal streaming PNG decoder producing 1-bit Atkinson-dithered bitmaps.
// Ported from TrustyReader (Rust) to C++17.
//
// Streams row-by-row: reads chunks from ZipEntryInput, feeds IDAT data
// into a tinfl (miniz) zlib decompressor, reconstructs scanlines one at a
// time with PNG filter un-apply, then dithers to 1-bit output.
//
// Supported colour types: greyscale (1/2/4/8/16 bpp), RGB (8/16),
// palette (1/2/4/8), grey+alpha (8/16), RGBA (8/16).
// Interlaced (Adam7) images: only pass 1 is decoded (blocky but streaming,
// no full-image buffer required).
//
// Peak heap ≈ 7 KB (tinfl_decompressor) + 32 KB (LZ dict) + scanline buffers
// + output bitmap.

#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#include <algorithm>
#include <cstring>
#include <memory>

#include "../HeapLog.h"
#include "ImageDecoder.h"
#include "ZipReader.h"
#include "miniz.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

namespace microreader {

// ---------------------------------------------------------------------------
// PNG constants
// ---------------------------------------------------------------------------

static const uint8_t kPngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};

static constexpr uint8_t kColorGreyscale = 0;
static constexpr uint8_t kColorRGB = 2;
static constexpr uint8_t kColorPalette = 3;
static constexpr uint8_t kColorGreyAlpha = 4;
static constexpr uint8_t kColorRGBA = 6;

static constexpr uint8_t kFilterNone = 0;
static constexpr uint8_t kFilterSub = 1;
static constexpr uint8_t kFilterUp = 2;
static constexpr uint8_t kFilterAverage = 3;
static constexpr uint8_t kFilterPaeth = 4;

// With streaming ImageRowSink, we never hold the full decoded bitmap.
// The real memory cost is scanline buffers (proportional to width only).
// Keep this high enough for common ebook covers.
static constexpr uint32_t kMaxPixels = 8192u * 8192u;
static constexpr size_t kIDAT_BufSize = 4096;
static constexpr size_t kLZDictSize = 32768;  // must equal TINFL_LZ_DICT_SIZE

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool zip_read_exact(ZipEntryInput& inp, void* buf, size_t n) {
  size_t got = 0;
  auto* p = static_cast<uint8_t*>(buf);
  while (got < n) {
    size_t r = inp.read(p + got, n - got);
    if (r == 0)
      return false;
    got += r;
  }
  return true;
}

static bool zip_skip(ZipEntryInput& inp, size_t n) {
  uint8_t tmp[64];
  while (n > 0) {
    size_t chunk = n < sizeof(tmp) ? n : sizeof(tmp);
    size_t r = inp.read(tmp, chunk);
    if (r == 0)
      return false;
    n -= r;
  }
  return true;
}

static uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// ---------------------------------------------------------------------------
// PNG header info
// ---------------------------------------------------------------------------

struct PngHeader {
  uint32_t src_width = 0;
  uint32_t src_height = 0;
  uint8_t bit_depth = 0;
  uint8_t color_type = 0;

  // Number of bytes per pixel for filter stride (1 for sub-byte depths).
  int bytes_per_pixel() const {
    int channels = 1;
    switch (color_type) {
      case kColorGreyscale:
        channels = 1;
        break;
      case kColorRGB:
        channels = 3;
        break;
      case kColorPalette:
        channels = 1;
        break;
      case kColorGreyAlpha:
        channels = 2;
        break;
      case kColorRGBA:
        channels = 4;
        break;
    }
    if (bit_depth >= 8)
      return channels * (bit_depth / 8);
    return 1;  // sub-byte packed
  }

  // Byte length of one unfiltered scanline (without the leading filter byte).
  size_t scanline_bytes() const {
    size_t bpp = 0;
    switch (color_type) {
      case kColorGreyscale:
        bpp = bit_depth;
        break;
      case kColorRGB:
        bpp = 3 * bit_depth;
        break;
      case kColorPalette:
        bpp = bit_depth;
        break;
      case kColorGreyAlpha:
        bpp = 2 * bit_depth;
        break;
      case kColorRGBA:
        bpp = 4 * bit_depth;
        break;
      default:
        bpp = bit_depth;
        break;
    }
    return (src_width * bpp + 7) / 8;
  }
};

// ---------------------------------------------------------------------------
// Pixel → greyscale conversion
// ---------------------------------------------------------------------------

static uint8_t rgb_to_grey(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint8_t>((uint16_t(r) * 77 + uint16_t(g) * 150 + uint16_t(b) * 29) >> 8);
}

static uint8_t blend_white(uint8_t grey, uint8_t alpha) {
  uint16_t g = grey, a = alpha;
  return static_cast<uint8_t>((g * a + 255 * (255 - a)) / 255);
}

static uint8_t unpack_sub_byte(const uint8_t* row, size_t x, uint8_t bit_depth) {
  size_t ppb = 8u / bit_depth;
  size_t byte_idx = x / ppb;
  size_t bit_offset = (ppb - 1 - x % ppb) * bit_depth;
  uint8_t mask = static_cast<uint8_t>((1u << bit_depth) - 1u);
  uint8_t raw = (row[byte_idx] >> bit_offset) & mask;
  // Scale to 0-255
  uint16_t maxv = static_cast<uint16_t>((1u << bit_depth) - 1u);
  return static_cast<uint8_t>(uint16_t(raw) * 255u / maxv);
}

static uint8_t pixel_to_grey(const uint8_t* row, size_t x, const PngHeader& hdr, const uint8_t palette_grey[256]) {
  switch ((hdr.color_type << 5) | hdr.bit_depth) {
    // Greyscale
    case (kColorGreyscale << 5) | 8:
      return row[x];
    case (kColorGreyscale << 5) | 16:
      return row[x * 2];
    case (kColorGreyscale << 5) | 1:
    case (kColorGreyscale << 5) | 2:
    case (kColorGreyscale << 5) | 4:
      return unpack_sub_byte(row, x, hdr.bit_depth);

    // RGB
    case (kColorRGB << 5) | 8:
      return rgb_to_grey(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
    case (kColorRGB << 5) | 16:
      return rgb_to_grey(row[x * 6], row[x * 6 + 2], row[x * 6 + 4]);

    // Palette
    case (kColorPalette << 5) | 8:
      return palette_grey[row[x]];
    case (kColorPalette << 5) | 1:
    case (kColorPalette << 5) | 2:
    case (kColorPalette << 5) | 4: {
      size_t ppb = 8u / hdr.bit_depth;
      size_t byte_idx = x / ppb;
      size_t bit_off = (ppb - 1 - x % ppb) * hdr.bit_depth;
      uint8_t mask = static_cast<uint8_t>((1u << hdr.bit_depth) - 1u);
      uint8_t idx = (row[byte_idx] >> bit_off) & mask;
      return palette_grey[idx];
    }

    // Grey + alpha
    case (kColorGreyAlpha << 5) | 8:
      return blend_white(row[x * 2], row[x * 2 + 1]);
    case (kColorGreyAlpha << 5) | 16:
      return blend_white(row[x * 4], row[x * 4 + 2]);

    // RGBA
    case (kColorRGBA << 5) | 8: {
      uint8_t g = rgb_to_grey(row[x * 4], row[x * 4 + 1], row[x * 4 + 2]);
      return blend_white(g, row[x * 4 + 3]);
    }
    case (kColorRGBA << 5) | 16: {
      uint8_t g = rgb_to_grey(row[x * 8], row[x * 8 + 2], row[x * 8 + 4]);
      return blend_white(g, row[x * 8 + 6]);
    }

    default:
      return 128;
  }
}

// ---------------------------------------------------------------------------
// PNG filter reconstruction
// ---------------------------------------------------------------------------

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
  int16_t ia = a, ib = b, ic = c;
  int16_t p = ia + ib - ic;
  int16_t pa = static_cast<int16_t>(p > ia ? p - ia : ia - p);
  int16_t pb = static_cast<int16_t>(p > ib ? p - ib : ib - p);
  int16_t pc = static_cast<int16_t>(p > ic ? p - ic : ic - p);
  if (pa <= pb && pa <= pc)
    return a;
  if (pb <= pc)
    return b;
  return c;
}

static void unfilter_row(uint8_t filter, uint8_t* row, const uint8_t* prev, size_t len, int bpp) {
  size_t ib = static_cast<size_t>(bpp);
  switch (filter) {
    case kFilterNone:
      break;
    case kFilterSub:
      for (size_t i = ib; i < len; ++i)
        row[i] = static_cast<uint8_t>(row[i] + row[i - ib]);
      break;
    case kFilterUp: {
      // Word-parallel byte addition: 4 bytes per iteration, no cross-byte carry.
      // Correct on unaligned inputs via memcpy; GCC emits lw/sw when aligned.
      size_t i = 0;
      for (; i + 4 <= len; i += 4) {
        uint32_t r, p;
        std::memcpy(&r, row + i, 4);
        std::memcpy(&p, prev + i, 4);
        uint32_t even = ((r & 0x00FF00FFu) + (p & 0x00FF00FFu)) & 0x00FF00FFu;
        uint32_t odd = (((r >> 8) & 0x00FF00FFu) + ((p >> 8) & 0x00FF00FFu)) & 0x00FF00FFu;
        r = even | (odd << 8);
        std::memcpy(row + i, &r, 4);
      }
      for (; i < len; ++i)
        row[i] = static_cast<uint8_t>(row[i] + prev[i]);
      break;
    }
    case kFilterAverage: {
      // First ib bytes: left pixel (a) = 0.
      for (size_t i = 0; i < ib && i < len; ++i)
        row[i] = static_cast<uint8_t>(row[i] + static_cast<uint8_t>(uint16_t(prev[i]) / 2));
      for (size_t i = ib; i < len; ++i)
        row[i] = static_cast<uint8_t>(row[i] + static_cast<uint8_t>((uint16_t(row[i - ib]) + uint16_t(prev[i])) / 2));
      break;
    }
    case kFilterPaeth: {
      // First ib bytes: a = 0, c = 0 → paeth(0, b, 0) = b; hoists branch out of loop.
      for (size_t i = 0; i < ib && i < len; ++i)
        row[i] = static_cast<uint8_t>(row[i] + prev[i]);
      for (size_t i = ib; i < len; ++i) {
        uint8_t a = row[i - ib];
        uint8_t b = prev[i];
        uint8_t c = prev[i - ib];
        row[i] = static_cast<uint8_t>(row[i] + paeth(a, b, c));
      }
      break;
    }
    default:
      break;  // Unknown filter — treat as None
  }
}

// ---------------------------------------------------------------------------
// Atkinson dithering (one PNG row → 1-bit output)
// Distributes 6/8 of error to 6 neighbors; intentionally loses 1/4.
// Requires 3 error rows: cur (this row), nxt (next), nxt2 (row+2).
// ---------------------------------------------------------------------------

static void dither_row_png(const uint8_t* src_row, const PngHeader& hdr, const uint8_t palette_grey[256],
                           uint32_t x_step, int out_w, int16_t* err_cur, int16_t* err_nxt, int16_t* err_nxt2,
                           uint8_t* out_row) {
  uint32_t sx_fp = 0;
  uint8_t acc = 0, bit = 0x80;
  bool grey8 = (hdr.color_type == kColorGreyscale && hdr.bit_depth == 8);
  bool rgb8 = (hdr.color_type == kColorRGB && hdr.bit_depth == 8);
  bool pal4 = (hdr.color_type == kColorPalette && hdr.bit_depth == 4);
  for (int ox = 0; ox < out_w; ++ox) {
    size_t sx = sx_fp >> 16;
    sx_fp += x_step;
    int16_t g;
    if (grey8)
      g = static_cast<int16_t>(src_row[sx]);
    else if (rgb8)
      g = static_cast<int16_t>(rgb_to_grey(src_row[sx * 3], src_row[sx * 3 + 1], src_row[sx * 3 + 2]));
    else if (pal4) {
      uint8_t n = (sx & 1) ? (src_row[sx >> 1] & 0x0F) : (src_row[sx >> 1] >> 4);
      g = static_cast<int16_t>(palette_grey[n]);
    } else
      g = static_cast<int16_t>(pixel_to_grey(src_row, sx, hdr, palette_grey));

    int16_t val = static_cast<int16_t>(g + err_cur[ox + 1]);
    if (val < 0)
      val = 0;
    if (val > 255)
      val = 255;
    bool white = val >= 128;
    int16_t e = white ? static_cast<int16_t>(val - 255) : val;
    int16_t q = static_cast<int16_t>(e >> 3);

    if (white)
      acc |= bit;
    bit >>= 1;
    if (!bit) {
      *out_row++ = acc;
      acc = 0;
      bit = 0x80;
    }

    err_cur[ox + 2] = static_cast<int16_t>(err_cur[ox + 2] + q);
    err_cur[ox + 3] = static_cast<int16_t>(err_cur[ox + 3] + q);
    err_nxt[ox] = static_cast<int16_t>(err_nxt[ox] + q);
    err_nxt[ox + 1] = static_cast<int16_t>(err_nxt[ox + 1] + q);
    err_nxt[ox + 2] = static_cast<int16_t>(err_nxt[ox + 2] + q);
    err_nxt2[ox + 1] = static_cast<int16_t>(err_nxt2[ox + 1] + q);
  }
  if (bit != 0x80)
    *out_row = acc;
}

ImageError decode_png_from_entry(IZipFile& file, const ZipEntry& entry, uint16_t max_w, uint16_t max_h,
                                 DecodedImage& out, uint8_t* work_buf, size_t work_buf_size, bool scale_to_fill,
                                 ImageRowSink* sink, ImagePixelSink* pixel_sink) {
  // If no work_buf, allocate one
#ifdef ESP_PLATFORM
  int64_t _t_entry = esp_timer_get_time();
#endif
  std::unique_ptr<uint8_t[]> owned_work;
  if (!work_buf || work_buf_size < ZipEntryInput::kMinWorkBufSize) {
    owned_work = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[ZipEntryInput::kMinWorkBufSize]);
    if (!owned_work)
      return ImageError::ReadError;
    work_buf = owned_work.get();
    work_buf_size = ZipEntryInput::kMinWorkBufSize;
  }

  ZipEntryInput inp;
  if (inp.open(file, entry, work_buf, work_buf_size) != ZipError::Ok)
    return ImageError::ReadError;

  // ---- PNG signature ----
  uint8_t sig[8];
  if (!zip_read_exact(inp, sig, 8) || std::memcmp(sig, kPngSig, 8) != 0)
    return ImageError::InvalidData;

  // ---- IHDR ----
  uint8_t chunk_hdr[8];  // 4-byte length + 4-byte type
  if (!zip_read_exact(inp, chunk_hdr, 8))
    return ImageError::InvalidData;
  uint32_t ihdr_len = be32(chunk_hdr);
  if (ihdr_len < 13 || std::memcmp(chunk_hdr + 4, "IHDR", 4) != 0)
    return ImageError::InvalidData;
  uint8_t ihdr_raw[13];
  if (!zip_read_exact(inp, ihdr_raw, 13))
    return ImageError::InvalidData;
  if (ihdr_len > 13 && !zip_skip(inp, ihdr_len - 13))
    return ImageError::ReadError;
  // Skip IHDR CRC
  if (!zip_skip(inp, 4))
    return ImageError::ReadError;

  PngHeader hdr;
  hdr.src_width = be32(ihdr_raw);
  hdr.src_height = be32(ihdr_raw + 4);
  hdr.bit_depth = ihdr_raw[8];
  hdr.color_type = ihdr_raw[9];
  uint8_t interlace = ihdr_raw[12];

  if (!hdr.src_width || !hdr.src_height)
    return ImageError::InvalidData;
  if (interlace != 0 && interlace != 1)
    return ImageError::UnsupportedFormat;
  if (hdr.src_width * hdr.src_height > kMaxPixels)
    return ImageError::TooLarge;

  // Validate colour type + bit depth combination
  switch (hdr.color_type) {
    case kColorGreyscale:
      if (hdr.bit_depth != 1 && hdr.bit_depth != 2 && hdr.bit_depth != 4 && hdr.bit_depth != 8 && hdr.bit_depth != 16)
        return ImageError::InvalidData;
      break;
    case kColorRGB:
      if (hdr.bit_depth != 8 && hdr.bit_depth != 16)
        return ImageError::InvalidData;
      break;
    case kColorPalette:
      if (hdr.bit_depth != 1 && hdr.bit_depth != 2 && hdr.bit_depth != 4 && hdr.bit_depth != 8)
        return ImageError::InvalidData;
      break;
    case kColorGreyAlpha:
      if (hdr.bit_depth != 8 && hdr.bit_depth != 16)
        return ImageError::InvalidData;
      break;
    case kColorRGBA:
      if (hdr.bit_depth != 8 && hdr.bit_depth != 16)
        return ImageError::InvalidData;
      break;
    default:
      return ImageError::UnsupportedFormat;
  }

  // ---- Scan for PLTE/tRNS, then first IDAT ----
  uint8_t palette_grey[256] = {};
  uint8_t palette_alpha[256];
  std::memset(palette_alpha, 255, sizeof(palette_alpha));
  uint32_t first_idat_len = 0;
  for (;;) {
    if (!zip_read_exact(inp, chunk_hdr, 8))
      return ImageError::InvalidData;
    uint32_t clen = be32(chunk_hdr);
    uint8_t* ctype = chunk_hdr + 4;

    if (std::memcmp(ctype, "IDAT", 4) == 0) {
      first_idat_len = clen;
      break;
    } else if (std::memcmp(ctype, "PLTE", 4) == 0 && clen <= 768 && clen % 3 == 0) {
      // Build greyscale LUT from palette
      uint8_t plte_scratch[768];
      size_t to_read = clen < sizeof(plte_scratch) ? clen : sizeof(plte_scratch);
      if (!zip_read_exact(inp, plte_scratch, to_read))
        return ImageError::ReadError;
      if (clen > sizeof(plte_scratch))
        zip_skip(inp, clen - sizeof(plte_scratch));
      // CRC
      if (!zip_skip(inp, 4))
        return ImageError::ReadError;
      for (size_t i = 0; i < to_read / 3; ++i) {
        palette_grey[i] = rgb_to_grey(plte_scratch[i * 3], plte_scratch[i * 3 + 1], plte_scratch[i * 3 + 2]);
      }
    } else if (std::memcmp(ctype, "tRNS", 4) == 0) {
      if (hdr.color_type == kColorPalette && clen <= 256) {
        // Per-entry alpha for palette images
        uint8_t trns[256];
        if (!zip_read_exact(inp, trns, clen))
          return ImageError::ReadError;
        std::memcpy(palette_alpha, trns, clen);
        // entries beyond clen remain fully opaque (255)
      } else {
        if (!zip_skip(inp, clen))
          return ImageError::ReadError;
      }
      // CRC
      if (!zip_skip(inp, 4))
        return ImageError::ReadError;
    } else {
      if (!zip_skip(inp, clen + 4))
        return ImageError::ReadError;
    }
  }

  // Apply tRNS alpha to palette: blend each entry against white background.
  if (hdr.color_type == kColorPalette) {
    for (int i = 0; i < 256; ++i) {
      if (palette_alpha[i] != 255)
        palette_grey[i] = blend_white(palette_grey[i], palette_alpha[i]);
    }
  }

  // ---- Compute output dimensions ----
  uint32_t src_w = hdr.src_width, src_h = hdr.src_height;
  if (max_w == 0)
    max_w = static_cast<uint16_t>(src_w < 65535 ? src_w : 65535);
  if (max_h == 0)
    max_h = static_cast<uint16_t>(src_h < 65535 ? src_h : 65535);

  uint32_t out_w, out_h;
  if (scale_to_fill) {
    // Caller already computed correct aspect-ratio-preserving target.
    out_w = max_w;
    out_h = max_h;
  } else if (src_w <= max_w && src_h <= max_h) {
    out_w = src_w;
    out_h = src_h;
  } else if (src_w * uint64_t(max_h) > src_h * uint64_t(max_w)) {
    out_w = max_w;
    out_h = std::max(uint32_t(1), src_h * uint32_t(max_w) / src_w);
  } else {
    out_h = max_h;
    out_w = std::max(uint32_t(1), src_w * uint32_t(max_h) / src_h);
  }
  uint32_t x_step = (src_w << 16) / out_w;
  uint32_t y_step = (src_h << 16) / out_h;
  uint32_t out_stride = (out_w + 7) / 8;

  // ---- Adam7 interlaced path ----
  // Decodes all 7 passes in a single IDAT stream pass.  Each decoded pixel is
  // written immediately via pixel_sink (random-access).  No output buffer is
  // allocated — memory cost is 2 scanlines + 32 KB LZ dict, same as the
  // non-interlaced path.
  //
  // If pixel_sink is null we fall back to allocating an output buffer and
  // emitting rows at the end (or using row_sink if provided).
  if (interlace == 1) {
    // Adam7 pass parameters (XStart, YStart, XDelta, YDelta)
    static const uint8_t kA7XS[7] = {0, 4, 0, 2, 0, 1, 0};
    static const uint8_t kA7YS[7] = {0, 0, 4, 0, 2, 0, 1};
    static const uint8_t kA7XD[7] = {8, 8, 4, 4, 2, 2, 1};
    static const uint8_t kA7YD[7] = {8, 8, 8, 4, 4, 2, 2};

    // Bayer 4×4 ordered dithering (pixel-independent, no error rows needed)
    static const uint8_t kBayer[4][4] = {
        {0,   128, 32,  160},
        {192, 64,  224, 96 },
        {48,  176, 16,  144},
        {240, 112, 208, 80 }
    };

    // --- fallback: no pixel_sink → allocate full output buffer ---
    std::unique_ptr<uint8_t[]> a7_out;
    if (!pixel_sink) {
      a7_out.reset(new (std::nothrow) uint8_t[out_stride * out_h]);
      if (!a7_out)
        return ImageError::ReadError;
      std::fill(a7_out.get(), a7_out.get() + out_stride * out_h, uint8_t(0xFF));
    }

    // Scanline buffers sized for the widest pass (pass 7, width = src_width)
    size_t max_scan_bytes = hdr.scanline_bytes();
    int bpp_filter_a7 = hdr.bytes_per_pixel();
    auto prev_row_a7 = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[max_scan_bytes]());
    auto curr_row_a7 = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[max_scan_bytes]());
    auto row_buf_a7 = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[1 + max_scan_bytes]);
    if (!prev_row_a7 || !curr_row_a7 || !row_buf_a7)
      return ImageError::ReadError;

    auto decomp_a7 = std::unique_ptr<tinfl_decompressor>(new (std::nothrow) tinfl_decompressor{});
    if (!decomp_a7)
      return ImageError::ReadError;
    tinfl_init(decomp_a7.get());
    auto lz_dict_a7 = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[kLZDictSize]());
    if (!lz_dict_a7)
      return ImageError::ReadError;

    // Helper: scanline byte count for a given pass (using its reduced width)
    auto pass_scan_bytes = [&](int p) -> size_t {
      uint32_t pw = src_w > kA7XS[p] ? (src_w - kA7XS[p] + kA7XD[p] - 1) / kA7XD[p] : 0;
      PngHeader ph2 = hdr;
      ph2.src_width = pw;
      return ph2.scanline_bytes();
    };

    uint8_t idat_buf_a7[kIDAT_BufSize];
    size_t in_avail_a7 = 0;
    size_t idat_chunk_left_a7 = first_idat_len;
    bool more_idat_a7 = true;
    size_t dict_pos_a7 = 0;
    size_t row_pos_a7 = 0;
    uint32_t pass_row_a7 = 0;

    // Find first non-empty pass
    int cur_pass = 0;
    while (cur_pass < 7) {
      uint32_t pw = src_w > kA7XS[cur_pass] ? (src_w - kA7XS[cur_pass] + kA7XD[cur_pass] - 1) / kA7XD[cur_pass] : 0;
      uint32_t ph = src_h > kA7YS[cur_pass] ? (src_h - kA7YS[cur_pass] + kA7YD[cur_pass] - 1) / kA7YD[cur_pass] : 0;
      if (pw > 0 && ph > 0)
        break;
      ++cur_pass;
    }
    size_t row_total_a7 = (cur_pass < 7) ? 1 + pass_scan_bytes(cur_pass) : 0;

    for (;;) {
      // Refill IDAT input buffer
      while (in_avail_a7 < kIDAT_BufSize) {
        if (idat_chunk_left_a7 > 0) {
          size_t space = kIDAT_BufSize - in_avail_a7;
          size_t want = idat_chunk_left_a7 < space ? idat_chunk_left_a7 : space;
          size_t got = inp.read(idat_buf_a7 + in_avail_a7, want);
          if (got == 0) {
            more_idat_a7 = false;
            idat_chunk_left_a7 = 0;
            break;
          }
          in_avail_a7 += got;
          idat_chunk_left_a7 -= got;
        } else if (more_idat_a7) {
          if (!zip_skip(inp, 4)) {
            more_idat_a7 = false;
            break;
          }
          uint8_t next_hdr_a7[8];
          if (!zip_read_exact(inp, next_hdr_a7, 8)) {
            more_idat_a7 = false;
            break;
          }
          if (std::memcmp(next_hdr_a7 + 4, "IDAT", 4) == 0)
            idat_chunk_left_a7 = be32(next_hdr_a7);
          else {
            more_idat_a7 = false;
            break;
          }
        } else
          break;
      }

      bool has_more_a7 = (idat_chunk_left_a7 > 0) || more_idat_a7;
      mz_uint32 flags_a7 = TINFL_FLAG_PARSE_ZLIB_HEADER;
      if (has_more_a7)
        flags_a7 |= TINFL_FLAG_HAS_MORE_INPUT;

      size_t write_pos_a7 = dict_pos_a7 & (kLZDictSize - 1);
      size_t in_bytes_a7 = in_avail_a7;
      size_t out_bytes_a7 = kLZDictSize - write_pos_a7;

      tinfl_status status_a7 = tinfl_decompress(decomp_a7.get(), idat_buf_a7, &in_bytes_a7, lz_dict_a7.get(),
                                                lz_dict_a7.get() + write_pos_a7, &out_bytes_a7, flags_a7);
      if (in_bytes_a7 > 0 && in_bytes_a7 < in_avail_a7)
        std::memmove(idat_buf_a7, idat_buf_a7 + in_bytes_a7, in_avail_a7 - in_bytes_a7);
      in_avail_a7 -= in_bytes_a7;

      // Feed decompressed bytes into the current pass row buffer
      const uint8_t* src_d = lz_dict_a7.get() + write_pos_a7;
      size_t remaining_a7 = out_bytes_a7;
      while (remaining_a7 > 0 && cur_pass < 7) {
        size_t chunk = std::min(remaining_a7, row_total_a7 - row_pos_a7);
        std::memcpy(row_buf_a7.get() + row_pos_a7, src_d, chunk);
        row_pos_a7 += chunk;
        src_d += chunk;
        remaining_a7 -= chunk;

        if (row_pos_a7 == row_total_a7) {
          size_t pscan = row_total_a7 - 1;
          uint8_t filter_a7 = row_buf_a7[0];
          std::memcpy(curr_row_a7.get(), row_buf_a7.get() + 1, pscan);
          unfilter_row(filter_a7, curr_row_a7.get(), prev_row_a7.get(), pscan, bpp_filter_a7);

          uint32_t pw = (src_w - kA7XS[cur_pass] + kA7XD[cur_pass] - 1) / kA7XD[cur_pass];
          uint32_t src_row_y = kA7YS[cur_pass] + pass_row_a7 * kA7YD[cur_pass];
          uint32_t oy = src_row_y * out_h / src_h;

          if (oy < out_h) {
            uint8_t* out_row_ptr = a7_out ? a7_out.get() + oy * out_stride : nullptr;
            for (uint32_t pc = 0; pc < pw; ++pc) {
              uint32_t src_x = kA7XS[cur_pass] + pc * kA7XD[cur_pass];
              uint32_t ox = src_x * out_w / src_w;
              if (ox < out_w) {
                uint8_t g = pixel_to_grey(curr_row_a7.get(), pc, hdr, palette_grey);
                bool white = g >= kBayer[oy & 3][ox & 3];
                if (pixel_sink) {
                  pixel_sink->set_pixel(pixel_sink->ctx, static_cast<uint16_t>(ox), static_cast<uint16_t>(oy), white);
                } else {
                  uint8_t bit = static_cast<uint8_t>(0x80u >> (ox & 7));
                  if (white)
                    out_row_ptr[ox / 8] |= bit;
                  else
                    out_row_ptr[ox / 8] &= static_cast<uint8_t>(~bit);
                }
              }
            }
          }

          std::swap(prev_row_a7, curr_row_a7);
          std::fill(curr_row_a7.get(), curr_row_a7.get() + pscan, uint8_t(0));
          row_pos_a7 = 0;
          ++pass_row_a7;

          // Advance to next non-empty pass when this one is complete
          uint32_t ph = (src_h - kA7YS[cur_pass] + kA7YD[cur_pass] - 1) / kA7YD[cur_pass];
          if (pass_row_a7 >= ph) {
            ++cur_pass;
            while (cur_pass < 7) {
              uint32_t npw =
                  src_w > kA7XS[cur_pass] ? (src_w - kA7XS[cur_pass] + kA7XD[cur_pass] - 1) / kA7XD[cur_pass] : 0;
              uint32_t nph =
                  src_h > kA7YS[cur_pass] ? (src_h - kA7YS[cur_pass] + kA7YD[cur_pass] - 1) / kA7YD[cur_pass] : 0;
              if (npw > 0 && nph > 0)
                break;
              ++cur_pass;
            }
            if (cur_pass < 7) {
              row_total_a7 = 1 + pass_scan_bytes(cur_pass);
              pass_row_a7 = 0;
              std::fill(prev_row_a7.get(), prev_row_a7.get() + max_scan_bytes, uint8_t(0));
            }
          }
        }
      }

      dict_pos_a7 += out_bytes_a7;
      if (status_a7 == TINFL_STATUS_DONE)
        break;
      if (status_a7 < TINFL_STATUS_DONE)
        return ImageError::InvalidData;
      if (status_a7 == TINFL_STATUS_NEEDS_MORE_INPUT && !has_more_a7 && in_avail_a7 == 0)
        return ImageError::InvalidData;
    }

    out.width = static_cast<uint16_t>(out_w);
    out.height = static_cast<uint16_t>(out_h);
    if (!pixel_sink) {
      // Emit buffered output via row_sink or store in out.data
      if (sink) {
        for (uint32_t ay = 0; ay < out_h; ++ay)
          sink->emit_row(sink->ctx, static_cast<uint16_t>(ay), a7_out.get() + ay * out_stride,
                         static_cast<uint16_t>(out_w));
      } else {
        out.data.assign(a7_out.get(), a7_out.get() + out_stride * out_h);
      }
    }
    return ImageError::Ok;
  }

  // ---- Allocate working buffers ----
  size_t scan_bytes = hdr.scanline_bytes();
  int bpp_filter = hdr.bytes_per_pixel();

  // Two scanline buffers + one row_buf (scanline + filter byte)
  auto prev_row = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[scan_bytes]());
  auto curr_row = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[scan_bytes]());
  auto row_buf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[1 + scan_bytes]);
  if (!prev_row || !curr_row || !row_buf)
    return ImageError::ReadError;

  auto err_cur = std::unique_ptr<int16_t[]>(new (std::nothrow) int16_t[out_w + 4]());
  auto err_nxt = std::unique_ptr<int16_t[]>(new (std::nothrow) int16_t[out_w + 4]());
  auto err_nxt2 = std::unique_ptr<int16_t[]>(new (std::nothrow) int16_t[out_w + 4]());
  if (!err_cur || !err_nxt || !err_nxt2)
    return ImageError::ReadError;

  // Output bitmap
  out.width = static_cast<uint16_t>(out_w);
  out.height = static_cast<uint16_t>(out_h);
  if (!sink) {
    out.data.resize(static_cast<size_t>(out_stride) * static_cast<size_t>(out_h));
    std::fill(out.data.begin(), out.data.end(), uint8_t(0));
  }

  // tinfl decompressor + 32KB LZ dictionary on heap
  auto decomp_mem = std::unique_ptr<tinfl_decompressor>(new (std::nothrow) tinfl_decompressor{});
  if (!decomp_mem)
    return ImageError::ReadError;
  tinfl_init(decomp_mem.get());

  auto lz_dict = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[kLZDictSize]());
  if (!lz_dict)
    return ImageError::ReadError;

  // Small IDAT input buffer (stack is fine at 4 KB)
  uint8_t idat_buf[kIDAT_BufSize];
  size_t in_avail = 0;
  size_t idat_chunk_left = first_idat_len;
  bool more_idat = true;
  size_t dict_pos = 0;
  size_t row_pos = 0;
  size_t row_total = 1 + scan_bytes;
  uint32_t src_y = 0, out_y = 0;

  // ---- IDAT streaming decompression + row processing ----
#ifdef ESP_PLATFORM
  int64_t _t_idat = esp_timer_get_time();
  int64_t _t_pre = _t_idat - _t_entry;
#endif
  for (;;) {
    // Top up input buffer from the IDAT stream
    while (in_avail < kIDAT_BufSize) {
      if (idat_chunk_left > 0) {
        size_t space = kIDAT_BufSize - in_avail;
        size_t want = idat_chunk_left < space ? idat_chunk_left : space;
        size_t got = inp.read(idat_buf + in_avail, want);
        if (got == 0) {
          more_idat = false;
          idat_chunk_left = 0;
          break;
        }
        in_avail += got;
        idat_chunk_left -= got;
      } else if (more_idat) {
        // Skip CRC (4 bytes) and read next chunk header (8 bytes)
        if (!zip_skip(inp, 4)) {
          more_idat = false;
          break;
        }
        uint8_t next_hdr[8];
        if (!zip_read_exact(inp, next_hdr, 8)) {
          more_idat = false;
          break;
        }
        if (std::memcmp(next_hdr + 4, "IDAT", 4) == 0) {
          idat_chunk_left = be32(next_hdr);
        } else {
          more_idat = false;
          break;
        }
      } else {
        break;
      }
    }

    bool has_more = (idat_chunk_left > 0) || more_idat;
    mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
    if (has_more)
      flags |= TINFL_FLAG_HAS_MORE_INPUT;

    size_t write_pos = dict_pos & (kLZDictSize - 1);
    size_t in_bytes = in_avail;
    size_t out_bytes = kLZDictSize - write_pos;

    tinfl_status status = tinfl_decompress(decomp_mem.get(), idat_buf, &in_bytes, lz_dict.get(),
                                           lz_dict.get() + write_pos, &out_bytes, flags);

    if (in_bytes > 0 && in_bytes < in_avail)
      std::memmove(idat_buf, idat_buf + in_bytes, in_avail - in_bytes);
    in_avail -= in_bytes;

    // The decompressed region [write_pos, write_pos+out_bytes) is contiguous (no ring
    // wrap) because tinfl writes at most kLZDictSize-write_pos bytes per call.
    // Batch memcpy into row_buf instead of copying byte-at-a-time.
    {
      const uint8_t* src = lz_dict.get() + write_pos;
      size_t remaining = out_bytes;
      while (remaining > 0) {
        size_t chunk = std::min(remaining, row_total - row_pos);
        std::memcpy(row_buf.get() + row_pos, src, chunk);
        row_pos += chunk;
        src += chunk;
        remaining -= chunk;

        if (row_pos == row_total) {
          uint8_t filter = row_buf[0];
          std::memcpy(curr_row.get(), row_buf.get() + 1, scan_bytes);
          unfilter_row(filter, curr_row.get(), prev_row.get(), scan_bytes, bpp_filter);

          // Emit all output rows that map to this source row (handles upscaling too).
          while (out_y < out_h) {
            uint32_t target_src_y = (out_y * y_step) >> 16;
            if (src_y != target_src_y)
              break;
            if (sink) {
              uint8_t temp_row[128];  // byte accumulator writes all bytes
              dither_row_png(curr_row.get(), hdr, palette_grey, x_step, static_cast<int>(out_w), err_cur.get(),
                             err_nxt.get(), err_nxt2.get(), temp_row);
              sink->emit_row(sink->ctx, static_cast<uint16_t>(out_y), temp_row, static_cast<uint16_t>(out_w));
            } else {
              uint8_t* out_row = out.data.data() + out_y * out_stride;
              dither_row_png(curr_row.get(), hdr, palette_grey, x_step, static_cast<int>(out_w), err_cur.get(),
                             err_nxt.get(), err_nxt2.get(), out_row);
            }
            ++out_y;
            // Rotate three error rows: cur←nxt, nxt←nxt2, nxt2←fresh
            std::swap(err_cur, err_nxt);
            std::swap(err_nxt, err_nxt2);
            std::fill(err_nxt2.get(), err_nxt2.get() + out_w + 4, int16_t(0));
          }

          std::swap(prev_row, curr_row);
          std::fill(curr_row.get(), curr_row.get() + scan_bytes, uint8_t(0));
          row_pos = 0;
          ++src_y;
        }  // end row_pos == row_total
      }  // end while remaining
    }  // end batch copy block

    dict_pos += out_bytes;

    if (status == TINFL_STATUS_DONE)
      break;
    if (status < TINFL_STATUS_DONE)
      return ImageError::InvalidData;  // decompression error
    if (status == TINFL_STATUS_NEEDS_MORE_INPUT && !has_more && in_avail == 0)
      return ImageError::InvalidData;  // truncated IDAT
  }

  out.height = static_cast<uint16_t>(out_y);
#ifdef ESP_PLATFORM
  MR_LOGI("pngd", "ct=%u bd=%u src=%ux%u pre=%ldms idat=%ldms", (unsigned)hdr.color_type, (unsigned)hdr.bit_depth,
          (unsigned)hdr.src_width, (unsigned)hdr.src_height, (long)(_t_pre / 1000),
          (long)((esp_timer_get_time() - _t_idat) / 1000));
#endif
  return ImageError::Ok;
}

}  // namespace microreader
