#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ZipReader.h"

namespace microreader {

// Decoded image: 1-bit packed bitmap, MSB first, row-major.
struct DecodedImage {
  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<uint8_t> data;  // packed 1-bit, stride = (width+7)/8 bytes per row

  size_t stride() const {
    return (width + 7) / 8;
  }
  size_t data_size() const {
    return stride() * height;
  }

  // Get pixel at (x, y): returns white (true) for out-of-bounds coords.
  bool pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height)
      return true;
    size_t byte_idx = static_cast<size_t>(y) * stride() + static_cast<size_t>(x) / 8;
    uint8_t bit = 7 - (x % 8);
    return (data[byte_idx] >> bit) & 1;
  }
};

enum class ImageFormat {
  Unknown,
  Jpeg,
  Png,
};

// Errors from image operations.
enum class ImageError {
  Ok = 0,
  UnsupportedFormat,
  InvalidData,
  ReadError,
  TooLarge,
};

// Guess format from filename extension.
ImageFormat guess_format(const char* filename);

// Guess format from magic bytes (first 2+ bytes of data).
ImageFormat guess_format_from_magic(const uint8_t* data, size_t size);

// Read image dimensions from raw data without full decoding.
// Works for JPEG (scans for SOF marker) and PNG (reads IHDR).
ImageError read_image_size(const uint8_t* data, size_t size, uint16_t& width, uint16_t& height);

// Streaming image size parser — ~32 bytes of state, no heap allocation.
// Feed decompressed data chunks until feed() returns true, then check ok().
// Handles JPEG (scans past APPn/EXIF/ICC segments to SOF) and PNG (reads IHDR).
// PNG needs 24 bytes; JPEG SOF varies (typically 1-25KB with EXIF/ICC profiles).
struct ImageSizeStream {
  ImageSizeStream() = default;

  // Feed a data chunk. Returns true when done (success OR unsupported format).
  // After true: call ok() to check success, width()/height() for dimensions.
  bool feed(const uint8_t* data, size_t size);

  bool ok() const {
    return state_ == State::Done;
  }
  uint16_t width() const {
    return w_;
  }
  uint16_t height() const {
    return h_;
  }

 private:
  enum class State : uint8_t {
    Detect,    // inspect first 2 bytes to determine format
    PngSkip,   // skip bytes 2–15 (sig tail + IHDR length + "IHDR" tag)
    PngSize,   // read 8 bytes: width[4] height[4]
    JpegScan,  // scan forward for 0xFF marker byte
    JpegMark,  // read marker byte following 0xFF
    JpegLen1,  // read segment length high byte
    JpegLen2,  // read segment length low byte
    JpegSkip,  // skip segment payload bytes
    JpegSof,   // accumulate 7 SOF payload bytes (len16 + prec8 + height16 + width16)
    Done,
    Error,
  };

  State state_ = State::Detect;
  uint8_t detect_n_ = 0;
  uint8_t detect_[2] = {};
  uint8_t skip_left_ = 0;  // PNG: bytes remaining in PngSkip
  uint8_t size_[8] = {};   // PNG: accumulates width+height bytes
  uint8_t size_n_ = 0;
  uint32_t seg_left_ = 0;  // JPEG: bytes remaining to skip in current segment
  uint8_t sof_[7] = {};    // JPEG: SOF payload buffer
  uint8_t sof_n_ = 0;
  uint16_t w_ = 0, h_ = 0;
};

// Compute aspect-ratio-preserving scaled dimensions.
// Returns dimensions that fit within max_w × max_h.
void scaled_size(uint16_t raw_w, uint16_t raw_h, uint16_t max_w, uint16_t max_h, uint16_t& out_w, uint16_t& out_h);

// Read image dimensions from raw data without full decoding.
// Uses ImageSizeStream internally. Returns true if successful.
bool get_image_size(const uint8_t* data, size_t size, uint16_t& out_w, uint16_t& out_h);

// Runtime toggle: when false, image dimension resolution and decoding are skipped.
// Can be set from UI settings. Defaults to true.
extern bool images_enabled;

// Callback invoked for each dithered output row during streaming decode.
// y: 0-based row index within the image.
// data: packed 1-bit row, (width+7)/8 bytes, MSB=leftmost pixel.
// width: image width in pixels.
struct ImageRowSink {
  void (*emit_row)(void* ctx, uint16_t y, const uint8_t* data, uint16_t width) = nullptr;
  void* ctx = nullptr;
};

// Callback invoked per pixel during Adam7 interlaced PNG decode.
// Pixels arrive out of row order; the callback must support random writes.
// x, y: output pixel coordinates. white: true=white, false=black.
struct ImagePixelSink {
  void (*set_pixel)(void* ctx, uint16_t x, uint16_t y, bool white) = nullptr;
  void* ctx = nullptr;
};

// Decode an image to 1-bit dithered bitmap.
// Input: raw image data (JPEG or PNG).
// Output: DecodedImage with packed 1-bit pixels.
// max_w/max_h: maximum output dimensions (image is scaled to fit).
ImageError decode_image(const uint8_t* data, size_t size, uint16_t max_w, uint16_t max_h, DecodedImage& out);

// Decode a JPEG image by streaming directly from a ZIP entry.
// work_buf must be >= ZipEntryInput::kMinWorkBufSize for deflate entries;
// if nullptr (or too small), a buffer is heap-allocated internally.
// scale_to_fill: if true, scale up to fill max_w×max_h even when the source
// is smaller (aspect-ratio-preserving). Default false = cap to max, keep
// natural size for small images.
// sink: if non-null, each dithered row is emitted via the callback instead of
// being stored in out.data. out.width/height are still set.
ImageError decode_jpeg_from_entry(IZipFile& file, const ZipEntry& entry, uint16_t max_w, uint16_t max_h,
                                  DecodedImage& out, uint8_t* work_buf = nullptr, size_t work_buf_size = 0,
                                  bool scale_to_fill = false, ImageRowSink* sink = nullptr);

// Decode a PNG image by streaming directly from a ZIP entry.
// Same work_buf, scale_to_fill, and sink semantics as decode_jpeg_from_entry.
// pixel_sink: if non-null and the PNG is Adam7 interlaced, pixels are emitted
// via this random-access callback instead of row_sink (no output buffer needed).
ImageError decode_png_from_entry(IZipFile& file, const ZipEntry& entry, uint16_t max_w, uint16_t max_h,
                                 DecodedImage& out, uint8_t* work_buf = nullptr, size_t work_buf_size = 0,
                                 bool scale_to_fill = false, ImageRowSink* sink = nullptr,
                                 ImagePixelSink* pixel_sink = nullptr);

// Decode a JPEG or PNG image by streaming from a ZIP entry.
// Detects format from the first bytes of the entry.
// work_buf: optional caller-provided buffer (>= ZipEntryInput::kMinWorkBufSize).
// sink: if non-null, rows are emitted via callback (no out.data allocation).
// pixel_sink: if non-null and the PNG is Adam7 interlaced, pixels are emitted
// via this random-access callback instead of buffering the full output.
ImageError decode_image_from_entry(IZipFile& file, const ZipEntry& entry, uint16_t max_w, uint16_t max_h,
                                   DecodedImage& out, uint8_t* work_buf = nullptr, size_t work_buf_size = 0,
                                   bool scale_to_fill = false, ImageRowSink* sink = nullptr,
                                   ImagePixelSink* pixel_sink = nullptr);

// Floyd-Steinberg dither a grayscale buffer to 1-bit packed bitmap.
// grayscale: width*height bytes, 0=black, 255=white.
// out: pre-allocated packed bitmap, (width+7)/8 * height bytes.
void floyd_steinberg_dither(const uint8_t* grayscale, int width, int height, uint8_t* out);

}  // namespace microreader
