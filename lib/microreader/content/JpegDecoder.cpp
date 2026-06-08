// JpegDecoder.cpp
//
// Minimal streaming baseline JPEG decoder producing 1-bit Atkinson-dithered
// bitmaps. Ported from TrustyReader (Rust) to C++17.
//
// Single-pass header parse: markers are read segment-by-segment from a
// ZipEntryInput without buffering the whole header (avoids the 32KB header
// buffer used by the Rust reference). Peak heap ≈ 10 KB (JpegState + working
// buffers).  Output pixels are written directly into DecodedImage::data.
//
// Supported: baseline (SOF0), first-scan-only progressive (SOF2).
// Luminance (Y) channel decoded; Cb/Cr Huffman-decoded but discarded.

#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#include <algorithm>
#include <cstring>
#include <memory>

#include "../HeapLog.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#endif
#include "ImageDecoder.h"
#include "ZipReader.h"
#include "miniz.h"

namespace microreader {

// ---------------------------------------------------------------------------
// JPEG constants
// ---------------------------------------------------------------------------

static constexpr uint8_t M_SOF0 = 0xC0;
static constexpr uint8_t M_SOF2 = 0xC2;
static constexpr uint8_t M_DHT = 0xC4;
static constexpr uint8_t M_SOI = 0xD8;
static constexpr uint8_t M_EOI = 0xD9;
static constexpr uint8_t M_SOS = 0xDA;
static constexpr uint8_t M_DQT = 0xDB;
static constexpr uint8_t M_DRI = 0xDD;
static constexpr uint8_t M_RST0 = 0xD0;
static constexpr uint8_t M_RST7 = 0xD7;

static constexpr int MAX_COMP = 4;
// Sanity limit on source image total pixels.  With the streaming
// ImageRowSink approach, we never hold the full decoded bitmap — the
// real memory bottleneck is the y_row buffer (row_w * mcu_h), which
// has its own OOM check.  Keep this high enough for common ebook
// covers (up to ~3000×5000 = 15M pixels).
static constexpr uint32_t MAX_PIXELS = 8192u * 8192u;

// ---------------------------------------------------------------------------
// Zigzag scan order (zigzag index → natural 8×8 block position)
// ---------------------------------------------------------------------------

static constexpr uint8_t ZZ[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// ---------------------------------------------------------------------------
// IDCT constants (IJG ISLOW, CONST_BITS = 13)
// ---------------------------------------------------------------------------

static constexpr int32_t IDCT_CB = 13;
static constexpr int32_t IDCT_P1 = 2;
static constexpr int32_t F0298 = 2446;
static constexpr int32_t F0390 = 3196;
static constexpr int32_t F0541 = 4433;
static constexpr int32_t F0765 = 6270;
static constexpr int32_t F0899 = 7373;
static constexpr int32_t F1175 = 9633;
static constexpr int32_t F1501 = 12299;
static constexpr int32_t F1847 = 15137;
static constexpr int32_t F1961 = 16069;
static constexpr int32_t F2053 = 16819;
static constexpr int32_t F2562 = 20995;
static constexpr int32_t F3072 = 25172;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

struct Component {
  uint8_t id = 0;
  uint8_t h_samp = 1;
  uint8_t v_samp = 1;
  uint8_t qt_idx = 0;
  uint8_t dc_tbl = 0;
  uint8_t ac_tbl = 0;
};

// Huffman decoding table.
// sym[]/len[] are the 10-bit fast-path lookup: if len[x]>0, the code for
// the 10-bit pattern x has length len[x] and maps to symbol sym[x].
// mincode/maxcode/valptr are the slow path for codes longer than 10 bits.
static constexpr int kHuffLUTBits = 10;
static constexpr int kHuffLUTSize = 1 << kHuffLUTBits;  // 1024
struct HuffTable {
  uint8_t sym[kHuffLUTSize] = {};
  uint8_t len[kHuffLUTSize] = {};
  int32_t mincode[17] = {};
  int32_t maxcode[17] = {};  // -1 means no codes of this length
  uint16_t valptr[17] = {};
  uint8_t values[256] = {};
};

struct JpegState {
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t num_comp = 0;
  Component comp[MAX_COMP];
  uint8_t max_h = 1;
  uint8_t max_v = 1;
  uint16_t qt[4][64] = {};
  bool qt_ok[4] = {};
  HuffTable dc_huff[4];
  HuffTable ac_huff[4];
  bool dc_ok[4] = {};
  bool ac_ok[4] = {};
  uint16_t restart_interval = 0;
  uint8_t scan_num_comp = 0;
  uint8_t scan_order[MAX_COMP] = {};
  bool progressive = false;
  uint8_t scan_ss = 0;
  uint8_t scan_se = 63;
  uint8_t scan_al = 0;
};

// ---------------------------------------------------------------------------
// ZipEntryInput helpers
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
  uint8_t buf[64];
  while (n > 0) {
    size_t chunk = n < sizeof(buf) ? n : sizeof(buf);
    size_t r = inp.read(buf, chunk);
    if (r == 0)
      return false;
    n -= r;
  }
  return true;
}

static uint16_t be16(const uint8_t* p) {
  return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
}

// ---------------------------------------------------------------------------
// Marker segment parsers (operate on buffered segment payload)
// ---------------------------------------------------------------------------

static const char* parse_sof(const uint8_t* seg, size_t len, JpegState& st, bool prog) {
  if (len < 6)
    return "jpeg: SOF truncated";
  if (seg[0] != 8)
    return "jpeg: only 8-bit precision";
  st.height = be16(seg + 1);
  st.width = be16(seg + 3);
  st.num_comp = seg[5];
  st.progressive = prog;
  if (st.num_comp == 0 || st.num_comp > MAX_COMP)
    return "jpeg: bad component count";
  if (len < static_cast<size_t>(6 + st.num_comp * 3))
    return "jpeg: SOF truncated";
  st.max_h = 1;
  st.max_v = 1;
  for (int i = 0; i < st.num_comp; ++i) {
    st.comp[i].id = seg[6 + i * 3];
    uint8_t samp = seg[7 + i * 3];
    st.comp[i].h_samp = samp >> 4;
    st.comp[i].v_samp = samp & 0x0F;
    st.comp[i].qt_idx = seg[8 + i * 3];
    if (!st.comp[i].h_samp || !st.comp[i].v_samp)
      return "jpeg: zero sampling factor";
    if (st.comp[i].h_samp > st.max_h)
      st.max_h = st.comp[i].h_samp;
    if (st.comp[i].v_samp > st.max_v)
      st.max_v = st.comp[i].v_samp;
  }
  return nullptr;
}

static const char* parse_dqt(const uint8_t* seg, size_t len, JpegState& st) {
  size_t pos = 0;
  while (pos < len) {
    if (pos + 1 > len)
      return "jpeg: DQT truncated";
    uint8_t info = seg[pos++];
    uint8_t prec = info >> 4;
    uint8_t id = info & 0x0F;
    if (id >= 4)
      return "jpeg: DQT id OOB";
    if (prec == 0) {
      if (pos + 64 > len)
        return "jpeg: DQT truncated";
      for (int i = 0; i < 64; ++i)
        st.qt[id][i] = seg[pos++];
    } else {
      if (pos + 128 > len)
        return "jpeg: DQT truncated";
      for (int i = 0; i < 64; ++i) {
        st.qt[id][i] = be16(seg + pos);
        pos += 2;
      }
    }
    st.qt_ok[id] = true;
  }
  return nullptr;
}

static void build_huff_table(HuffTable& t, const uint8_t bits[16], const uint8_t* vals, size_t total) {
  std::memcpy(t.values, vals, total);
  std::memset(t.sym, 0, sizeof(t.sym));
  std::memset(t.len, 0, sizeof(t.len));
  std::fill(t.maxcode, t.maxcode + 17, int32_t(-1));
  std::memset(t.mincode, 0, sizeof(t.mincode));
  std::memset(t.valptr, 0, sizeof(t.valptr));

  uint32_t code = 0;
  size_t si = 0;
  for (int bl = 1; bl <= 16; ++bl) {
    int cnt = bits[bl - 1];
    if (cnt > 0) {
      t.valptr[bl] = static_cast<uint16_t>(si);
      t.mincode[bl] = static_cast<int32_t>(code);
      for (int k = 0; k < cnt; ++k) {
        if (bl <= kHuffLUTBits) {
          int prefix = static_cast<int>(code << (kHuffLUTBits - bl));
          int fill = 1 << (kHuffLUTBits - bl);
          for (int j = 0; j < fill && (prefix + j) < kHuffLUTSize; ++j) {
            t.sym[prefix + j] = vals[si];
            t.len[prefix + j] = static_cast<uint8_t>(bl);
          }
        }
        ++si;
        ++code;
      }
      t.maxcode[bl] = static_cast<int32_t>(code - 1);
    }
    code <<= 1;
  }
}

static const char* parse_dht(const uint8_t* seg, size_t len, JpegState& st) {
  size_t pos = 0;
  while (pos < len) {
    if (pos + 17 > len)
      return "jpeg: DHT truncated";
    uint8_t info = seg[pos++];
    uint8_t cls = info >> 4;
    uint8_t id = info & 0x0F;
    if (id >= 4)
      return "jpeg: DHT id OOB";
    uint8_t bits[16];
    std::memcpy(bits, seg + pos, 16);
    pos += 16;
    size_t total = 0;
    for (int i = 0; i < 16; ++i)
      total += bits[i];
    if (total > 256 || pos + total > len)
      return "jpeg: DHT value overflow";
    if (cls == 0) {
      build_huff_table(st.dc_huff[id], bits, seg + pos, total);
      st.dc_ok[id] = true;
    } else {
      build_huff_table(st.ac_huff[id], bits, seg + pos, total);
      st.ac_ok[id] = true;
    }
    pos += total;
  }
  return nullptr;
}

static const char* parse_sos(const uint8_t* seg, size_t len, JpegState& st) {
  if (len < 1)
    return "jpeg: SOS truncated";
  st.scan_num_comp = seg[0];
  if (st.scan_num_comp == 0 || st.scan_num_comp > st.num_comp)
    return "jpeg: SOS bad count";
  size_t needed = 1 + static_cast<size_t>(st.scan_num_comp) * 2 + 3;
  if (len < needed)
    return "jpeg: SOS truncated";
  for (int sci = 0; sci < st.scan_num_comp; ++sci) {
    uint8_t cs = seg[1 + sci * 2];
    uint8_t td_ta = seg[2 + sci * 2];
    bool found = false;
    for (int j = 0; j < st.num_comp; ++j) {
      if (st.comp[j].id == cs) {
        st.comp[j].dc_tbl = td_ta >> 4;
        st.comp[j].ac_tbl = td_ta & 0x0F;
        st.scan_order[sci] = static_cast<uint8_t>(j);
        found = true;
        break;
      }
    }
    if (!found)
      return "jpeg: SOS unknown component";
  }
  size_t off = 1 + static_cast<size_t>(st.scan_num_comp) * 2;
  st.scan_ss = seg[off];
  st.scan_se = seg[off + 1];
  st.scan_al = seg[off + 2] & 0x0F;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Single-pass header parser: reads segments from ZipEntryInput
// ---------------------------------------------------------------------------

// Max segment payload we will buffer; must accommodate the largest
// possible DQT/DHT/SOF segment. A single DHT of all 256 AC codes =
// 1 + 16 + 256 = 273 bytes. DQT of 4 precision-1 tables = 4*(1+128)=516.
// 2KB covers all valid cases with margin.
static constexpr size_t kMaxSegBuf = 2048;

static const char* parse_jpeg_header(ZipEntryInput& inp, JpegState& st) {
  // Check SOI
  uint8_t soi[2];
  if (!zip_read_exact(inp, soi, 2) || soi[0] != 0xFF || soi[1] != M_SOI)
    return "jpeg: invalid SOI";

  // Stack-allocated segment buffer
  uint8_t seg_buf[kMaxSegBuf];

  for (;;) {
    // Read marker: skip to next 0xFF, then read marker byte
    uint8_t b;
    do {
      if (!zip_read_exact(inp, &b, 1))
        return "jpeg: truncated reading for marker FF";
    } while (b != 0xFF);
    do {
      if (!zip_read_exact(inp, &b, 1))
        return "jpeg: truncated reading marker byte";
    } while (b == 0xFF);
    uint8_t marker = b;

    // Helper: read segment length and buffer payload
    auto read_segment = [&](size_t& payload_len) -> const char* {
      uint8_t lbuf[2];
      if (!zip_read_exact(inp, lbuf, 2))
        return "jpeg: truncated segment length";
      uint16_t seg_len = be16(lbuf);
      if (seg_len < 2)
        return "jpeg: bad segment length";
      payload_len = seg_len - 2;
      if (payload_len > kMaxSegBuf)
        return "jpeg: segment too large for buffer";
      if (payload_len > 0 && !zip_read_exact(inp, seg_buf, payload_len))
        return "jpeg: truncated segment data";
      return nullptr;
    };

    switch (marker) {
      case 0x00:
        // Stuffed byte (shouldn't appear outside entropy data) — skip
        continue;
      case M_SOI:
        // Restart or nested SOI — ignore
        continue;
      case M_EOI:
        return "jpeg: EOI before SOS";
      case M_RST0:
      case M_RST0 + 1:
      case M_RST0 + 2:
      case M_RST0 + 3:
      case M_RST0 + 4:
      case M_RST0 + 5:
      case M_RST0 + 6:
      case M_RST7:
        continue;  // standalone markers, no length field

      case M_SOF0:
      case M_SOF2: {
        size_t plen = 0;
        const char* e = read_segment(plen);
        if (e)
          return e;
        e = parse_sof(seg_buf, plen, st, marker == M_SOF2);
        if (e)
          return e;
        break;
      }
      // Unsupported SOF variants
      case 0xC1:
      case 0xC3:
      case 0xC5:
      case 0xC6:
      case 0xC7:
      case 0xC8:
      case 0xC9:
      case 0xCA:
      case 0xCB:
      case 0xCD:
      case 0xCE:
      case 0xCF:
        return "jpeg: unsupported SOF variant";

      case M_DHT: {
        // A single DHT segment may contain multiple tables but each
        // is at most ~273 bytes.  If the segment is larger than our
        // stack buffer (very unusual), fall through to skip.
        uint8_t lbuf[2];
        if (!zip_read_exact(inp, lbuf, 2))
          return "jpeg: truncated DHT length";
        uint16_t seg_len = be16(lbuf);
        if (seg_len < 2)
          return "jpeg: bad DHT length";
        size_t plen = seg_len - 2;
        if (plen > kMaxSegBuf) {
          // Extremely unusual; just skip — tables won't decode.
          if (!zip_skip(inp, plen))
            return "jpeg: truncated DHT skip";
          break;
        }
        if (plen > 0 && !zip_read_exact(inp, seg_buf, plen))
          return "jpeg: truncated DHT data";
        const char* e = parse_dht(seg_buf, plen, st);
        if (e)
          return e;
        break;
      }

      case M_DQT: {
        uint8_t lbuf[2];
        if (!zip_read_exact(inp, lbuf, 2))
          return "jpeg: truncated DQT length";
        uint16_t seg_len = be16(lbuf);
        if (seg_len < 2)
          return "jpeg: bad DQT length";
        size_t plen = seg_len - 2;
        if (plen > kMaxSegBuf)
          return "jpeg: DQT too large";
        if (plen > 0 && !zip_read_exact(inp, seg_buf, plen))
          return "jpeg: truncated DQT data";
        const char* e = parse_dqt(seg_buf, plen, st);
        if (e)
          return e;
        break;
      }

      case M_DRI: {
        // DRI: 2-byte length (= 4) + 2-byte restart interval
        uint8_t dri[4];
        if (!zip_read_exact(inp, dri, 4))
          return "jpeg: truncated DRI";
        st.restart_interval = be16(dri + 2);
        break;
      }

      case M_SOS: {
        uint8_t lbuf[2];
        if (!zip_read_exact(inp, lbuf, 2))
          return "jpeg: truncated SOS length";
        uint16_t seg_len = be16(lbuf);
        if (seg_len < 2)
          return "jpeg: bad SOS length";
        size_t plen = seg_len - 2;
        if (plen > kMaxSegBuf)
          return "jpeg: SOS too large";
        if (plen > 0 && !zip_read_exact(inp, seg_buf, plen))
          return "jpeg: truncated SOS data";
        const char* e = parse_sos(seg_buf, plen, st);
        if (e)
          return e;
        // Entropy data starts immediately after — return success.
        return nullptr;
      }

      default: {
        // Unknown segment: skip by length
        uint8_t lbuf[2];
        if (!zip_read_exact(inp, lbuf, 2))
          return "jpeg: truncated marker length";
        uint16_t seg_len = be16(lbuf);
        if (seg_len < 2)
          return "jpeg: bad marker length";
        size_t plen = seg_len - 2;
        if (plen > 0 && !zip_skip(inp, plen))
          return "jpeg: skip error";
        break;
      }
    }
  }
}

static const char* validate_tables(const JpegState& st) {
  for (int sci = 0; sci < st.scan_num_comp; ++sci) {
    int ci = st.scan_order[sci];
    const Component& c = st.comp[ci];
    if (!st.qt_ok[c.qt_idx])
      return "jpeg: missing quant table";
    if (!st.dc_ok[c.dc_tbl])
      return "jpeg: missing DC table";
    if (st.scan_se > 0 && !st.ac_ok[c.ac_tbl])
      return "jpeg: missing AC table";
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// BitReader — reads entropy-coded bytes from ZipEntryInput
// ---------------------------------------------------------------------------

class BitReader {
 public:
  explicit BitReader(ZipEntryInput& src) : src_(src) {}

  bool error() const {
    return error_;
  }

  // Peek n bits (msb-first) without consuming.
  uint32_t peek(uint8_t n) {
    ensure(n);
    return buf_ >> (32 - n);
  }

  // Consume n bits.
  void drop(uint8_t n) {
    buf_ <<= n;
    avail_ -= n;
  }

  // Read n bits and consume them.
  uint32_t read_bits(uint8_t n) {
    if (n == 0)
      return 0;
    ensure(n);
    uint32_t val = buf_ >> (32 - n);
    drop(n);
    return val;
  }

  // Synchronize after a restart marker: discard buffered bits and scan
  // forward to the next RST marker (or whatever comes next).
  void consume_restart() {
    buf_ = 0;
    avail_ = 0;
    if (stashed_marker_) {
      stashed_marker_ = 0;
      return;
    }
    for (;;) {
      uint8_t b = raw_byte();
      if (error_)
        return;
      if (b != 0xFF)
        continue;
      for (;;) {
        b = raw_byte();
        if (error_)
          return;
        if (b == 0xFF)
          continue;
        if (b == 0x00)
          break;  // stuffed byte in entropy stream
        return;   // RST or other marker — done
      }
    }
  }

 private:
  ZipEntryInput& src_;
  uint8_t cache_[512] = {};
  size_t cache_pos_ = 0;
  size_t cache_len_ = 0;
  uint32_t buf_ = 0;
  uint8_t avail_ = 0;
  uint8_t stashed_marker_ = 0;  // non-zero after hitting a marker in stream
  bool error_ = false;

  uint8_t raw_byte() {
    if (cache_pos_ >= cache_len_) {
      cache_len_ = src_.read(cache_, sizeof(cache_));
      cache_pos_ = 0;
      if (cache_len_ == 0) {
        error_ = true;
        return 0;
      }
    }
    return cache_[cache_pos_++];
  }

  // Next entropy-coded byte with JPEG byte-stuffing handling.
  uint8_t next_byte() {
    if (stashed_marker_)
      return 0;
    uint8_t b = raw_byte();
    if (error_)
      return 0;
    if (b != 0xFF)
      return b;
    for (;;) {
      uint8_t next = raw_byte();
      if (error_)
        return 0;
      if (next == 0x00)
        return 0xFF;  // stuffed 0xFF
      if (next == 0xFF)
        continue;  // padding FF bytes
      stashed_marker_ = next;
      return 0;
    }
  }

  void ensure(uint8_t n) {
    while (avail_ < n) {
      uint8_t b = next_byte();
      buf_ |= static_cast<uint32_t>(b) << (24 - avail_);
      avail_ = static_cast<uint8_t>(avail_ + 8);
    }
  }
};

// ---------------------------------------------------------------------------
// Huffman decode
// ---------------------------------------------------------------------------

static uint8_t huff_decode(BitReader& r, const HuffTable& t) {
  // 10-bit fast path: covers ~99% of codes in practice.
  uint32_t peek10 = r.peek(kHuffLUTBits);
  uint8_t nb = t.len[peek10];
  if (nb > 0) {
    r.drop(nb);
    return t.sym[peek10];
  }
  // Slow path: code > 10 bits (rare)
  uint32_t peek16 = r.peek(16);
  for (uint8_t bl = kHuffLUTBits + 1; bl <= 16; ++bl) {
    if (t.maxcode[bl] < 0)
      continue;
    int32_t code = static_cast<int32_t>(peek16 >> (16 - bl));
    if (code <= t.maxcode[bl]) {
      r.drop(bl);
      int32_t idx = t.valptr[bl] + code - t.mincode[bl];
      return t.values[static_cast<uint16_t>(idx)];
    }
  }
  // No valid code — signal error via stash (will surface as 0-bits)
  return 0;
}

static int32_t extend_val(uint32_t bits, uint8_t size) {
  uint32_t half = 1u << (size - 1);
  if (bits < half)
    return static_cast<int32_t>(bits) - static_cast<int32_t>((1u << size) - 1);
  return static_cast<int32_t>(bits);
}

// ---------------------------------------------------------------------------
// Block decoding
// ---------------------------------------------------------------------------

static const char* decode_block(BitReader& r, const HuffTable& dc_ht, const HuffTable& ac_ht, int32_t& dc_pred,
                                const uint16_t qt[64], int32_t blk[64], int se, uint8_t al) {
  std::memset(blk, 0, 64 * sizeof(int32_t));

  uint8_t dc_size = huff_decode(r, dc_ht);
  if (r.error())
    return "jpeg: DC Huffman read error";
  if (dc_size > 0) {
    if (dc_size > 11)
      return "jpeg: DC size > 11";
    uint32_t bits = r.read_bits(dc_size);
    dc_pred += extend_val(bits, dc_size);
  }
  blk[0] = (dc_pred << al) * static_cast<int32_t>(qt[0]);

  if (se > 0) {
    int k = 1;
    while (k <= se) {
      uint8_t sym = huff_decode(r, ac_ht);
      if (r.error())
        return "jpeg: AC Huffman read error";
      uint8_t run = sym >> 4;
      uint8_t size = sym & 0x0F;
      if (size == 0) {
        if (run == 15) {
          k += 16;
        }  // ZRL: skip 16 zeros
        else {
          break;
        }  // EOB
      } else {
        k += run;
        if (k > se)
          return "jpeg: AC index overflow";
        uint32_t bits = r.read_bits(size);
        int32_t val = extend_val(bits, size);
        blk[ZZ[k]] = (val << al) * static_cast<int32_t>(qt[k]);
        ++k;
      }
    }
  }
  return nullptr;
}

static const char* skip_block(BitReader& r, const HuffTable& dc_ht, const HuffTable& ac_ht, int32_t& dc_pred, int se) {
  uint8_t dc_size = huff_decode(r, dc_ht);
  if (r.error())
    return "jpeg: skip DC error";
  if (dc_size > 0) {
    uint32_t bits = r.read_bits(dc_size);
    dc_pred += extend_val(bits, dc_size);
  }
  if (se > 0) {
    int k = 1;
    while (k <= se) {
      uint8_t sym = huff_decode(r, ac_ht);
      if (r.error())
        return "jpeg: skip AC error";
      uint8_t run = sym >> 4;
      uint8_t size = sym & 0x0F;
      if (size == 0) {
        if (run == 15)
          k += 16;
        else
          break;
      } else {
        k += run + 1;
        r.read_bits(size);
      }
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// IDCT (IJG ISLOW, two-pass row + col)
// ---------------------------------------------------------------------------

static inline int32_t idct_descale(int32_t x, int32_t n) {
  return (x + (1 << (n - 1))) >> n;
}
static inline uint8_t idct_clamp(int32_t x) {
  return static_cast<uint8_t>(x < 0 ? 0 : (x > 255 ? 255 : x));
}

static void idct(const int32_t block[64], uint8_t out[64]) {
  int32_t ws[64];

  // Row pass
  for (int row = 0; row < 8; ++row) {
    const int b = row * 8;
    int32_t d0 = block[b], d1 = block[b + 1], d2 = block[b + 2], d3 = block[b + 3];
    int32_t d4 = block[b + 4], d5 = block[b + 5], d6 = block[b + 6], d7 = block[b + 7];

    if (!d1 && !d2 && !d3 && !d4 && !d5 && !d6 && !d7) {
      int32_t dc = d0 << IDCT_P1;
      ws[b] = ws[b + 1] = ws[b + 2] = ws[b + 3] = ws[b + 4] = ws[b + 5] = ws[b + 6] = ws[b + 7] = dc;
      continue;
    }

    int32_t z1 = (d2 + d6) * F0541;
    int32_t tmp2 = z1 + d6 * (-F1847);
    int32_t tmp3 = z1 + d2 * F0765;
    int32_t tmp0 = (d0 + d4) << IDCT_CB;
    int32_t tmp1 = (d0 - d4) << IDCT_CB;
    int32_t t10 = tmp0 + tmp3, t13 = tmp0 - tmp3;
    int32_t t11 = tmp1 + tmp2, t12 = tmp1 - tmp2;

    int32_t zz1 = d7 + d1, zz2 = d5 + d3, zz3 = d7 + d3, zz4 = d5 + d1;
    int32_t z5 = (zz3 + zz4) * F1175;
    int32_t o0 = d7 * F0298, o1 = d5 * F2053, o2 = d3 * F3072, o3 = d1 * F1501;
    int32_t s1 = zz1 * (-F0899), s2 = zz2 * (-F2562);
    int32_t s3 = zz3 * (-F1961) + z5, s4 = zz4 * (-F0390) + z5;
    o0 += s1 + s3;
    o1 += s2 + s4;
    o2 += s2 + s3;
    o3 += s1 + s4;

    int32_t sh = IDCT_CB - IDCT_P1;
    ws[b] = idct_descale(t10 + o3, sh);
    ws[b + 7] = idct_descale(t10 - o3, sh);
    ws[b + 1] = idct_descale(t11 + o2, sh);
    ws[b + 6] = idct_descale(t11 - o2, sh);
    ws[b + 2] = idct_descale(t12 + o1, sh);
    ws[b + 5] = idct_descale(t12 - o1, sh);
    ws[b + 3] = idct_descale(t13 + o0, sh);
    ws[b + 4] = idct_descale(t13 - o0, sh);
  }

  // Column pass
  for (int col = 0; col < 8; ++col) {
    int32_t d0 = ws[col], d1 = ws[col + 8], d2 = ws[col + 16], d3 = ws[col + 24];
    int32_t d4 = ws[col + 32], d5 = ws[col + 40], d6 = ws[col + 48], d7 = ws[col + 56];

    if (!d1 && !d2 && !d3 && !d4 && !d5 && !d6 && !d7) {
      uint8_t v = idct_clamp(idct_descale(d0, IDCT_P1 + 3) + 128);
      out[col] = out[col + 8] = out[col + 16] = out[col + 24] = v;
      out[col + 32] = out[col + 40] = out[col + 48] = out[col + 56] = v;
      continue;
    }

    int32_t z1 = (d2 + d6) * F0541;
    int32_t tmp2 = z1 + d6 * (-F1847);
    int32_t tmp3 = z1 + d2 * F0765;
    int32_t tmp0 = (d0 + d4) << IDCT_CB;
    int32_t tmp1 = (d0 - d4) << IDCT_CB;
    int32_t t10 = tmp0 + tmp3, t13 = tmp0 - tmp3;
    int32_t t11 = tmp1 + tmp2, t12 = tmp1 - tmp2;

    int32_t zz1 = d7 + d1, zz2 = d5 + d3, zz3 = d7 + d3, zz4 = d5 + d1;
    int32_t z5 = (zz3 + zz4) * F1175;
    int32_t o0 = d7 * F0298, o1 = d5 * F2053, o2 = d3 * F3072, o3 = d1 * F1501;
    int32_t s1 = zz1 * (-F0899), s2 = zz2 * (-F2562);
    int32_t s3 = zz3 * (-F1961) + z5, s4 = zz4 * (-F0390) + z5;
    o0 += s1 + s3;
    o1 += s2 + s4;
    o2 += s2 + s3;
    o3 += s1 + s4;

    int32_t sh = IDCT_CB + IDCT_P1 + 3;
    out[col] = idct_clamp(idct_descale(t10 + o3, sh) + 128);
    out[col + 56] = idct_clamp(idct_descale(t10 - o3, sh) + 128);
    out[col + 8] = idct_clamp(idct_descale(t11 + o2, sh) + 128);
    out[col + 48] = idct_clamp(idct_descale(t11 - o2, sh) + 128);
    out[col + 16] = idct_clamp(idct_descale(t12 + o1, sh) + 128);
    out[col + 40] = idct_clamp(idct_descale(t12 - o1, sh) + 128);
    out[col + 24] = idct_clamp(idct_descale(t13 + o0, sh) + 128);
    out[col + 32] = idct_clamp(idct_descale(t13 - o0, sh) + 128);
  }
}

// ---------------------------------------------------------------------------
// Atkinson dithering (one MCU row → 1-bit output)
// Distributes 6/8 of error to 6 neighbors; intentionally loses 1/4.
// Requires 3 error rows: cur (this row), nxt (next), nxt2 (row+2).
// ---------------------------------------------------------------------------

static void dither_row(const uint8_t* row, uint32_t x_step, int out_w, int16_t* err_cur, int16_t* err_nxt,
                       int16_t* err_nxt2, uint8_t* out_row) {
  uint32_t sx_fp = 0;
  uint8_t acc = 0, bit = 0x80;
  for (int ox = 0; ox < out_w; ++ox) {
    int16_t g = static_cast<int16_t>(row[sx_fp >> 16]);
    sx_fp += x_step;
    int16_t val = static_cast<int16_t>(g + err_cur[ox + 1]);
    if (val < 0)
      val = 0;
    if (val > 255)
      val = static_cast<int16_t>(255);
    bool white = val >= 128;
    int16_t e = white ? static_cast<int16_t>(val - 255) : val;
    int16_t q = static_cast<int16_t>(e >> 3);  // 1/8 of error

    if (white)
      acc |= bit;
    bit >>= 1;
    if (!bit) {
      *out_row++ = acc;
      acc = 0;
      bit = 0x80;
    }

    err_cur[ox + 2] = static_cast<int16_t>(err_cur[ox + 2] + q);    // (x+1, y)
    err_cur[ox + 3] = static_cast<int16_t>(err_cur[ox + 3] + q);    // (x+2, y)
    err_nxt[ox] = static_cast<int16_t>(err_nxt[ox] + q);            // (x-1, y+1)
    err_nxt[ox + 1] = static_cast<int16_t>(err_nxt[ox + 1] + q);    // (x,   y+1)
    err_nxt[ox + 2] = static_cast<int16_t>(err_nxt[ox + 2] + q);    // (x+1, y+1)
    err_nxt2[ox + 1] = static_cast<int16_t>(err_nxt2[ox + 1] + q);  // (x,   y+2)
  }
  if (bit != 0x80)
    *out_row = acc;
}

// ---------------------------------------------------------------------------
// Baseline decode
// ---------------------------------------------------------------------------

static const char* decode_baseline(const JpegState& st, BitReader& r, uint16_t max_w, uint16_t max_h, DecodedImage& out,
                                   bool scale_to_fill, ImageRowSink* sink) {
  int w = st.width, h = st.height;
  if (!w || !h)
    return "jpeg: zero dimensions";
  if (static_cast<uint32_t>(w) * static_cast<uint32_t>(h) > MAX_PIXELS)
    return "jpeg: exceeds pixel limit";

  // Compute output dimensions (aspect-ratio-preserving)
  int out_w, out_h;
  if (max_w == 0)
    max_w = static_cast<uint16_t>(w);
  if (max_h == 0)
    max_h = static_cast<uint16_t>(h);
  if (scale_to_fill) {
    // Caller already computed correct aspect-ratio-preserving target.
    out_w = max_w;
    out_h = max_h;
  } else if (w <= max_w && h <= max_h) {
    out_w = w;
    out_h = h;
  } else if (static_cast<uint32_t>(w) * max_h > static_cast<uint32_t>(h) * max_w) {
    out_w = max_w;
    out_h = std::max(1, static_cast<int>(static_cast<uint32_t>(h) * max_w / w));
  } else {
    out_h = max_h;
    out_w = std::max(1, static_cast<int>(static_cast<uint32_t>(w) * max_h / h));
  }

  uint32_t x_step = (static_cast<uint32_t>(w) << 16) / static_cast<uint32_t>(out_w);
  uint32_t y_step = (static_cast<uint32_t>(h) << 16) / static_cast<uint32_t>(out_h);
  int out_stride = (out_w + 7) / 8;

  // Non-interleaved scan (single component): MCU = 1 data unit (8×8 block).
  // Interleaved scan (multiple components): MCU = max_h×max_v blocks.
  bool non_interleaved = (st.scan_num_comp == 1);
  int mcu_w = non_interleaved ? 8 : st.max_h * 8;
  int mcu_h = non_interleaved ? 8 : st.max_v * 8;
  int mcus_x = (w + mcu_w - 1) / mcu_w;
  int mcus_y = (h + mcu_h - 1) / mcu_h;
  int row_w = mcus_x * mcu_w;

  // Allocate working buffers on heap to stay off the (small) stack
  auto y_row = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[row_w * mcu_h]);
  if (!y_row)
    return "jpeg: OOM for y_row";

  out.width = static_cast<uint16_t>(out_w);
  out.height = static_cast<uint16_t>(out_h);
  if (!sink) {
    size_t data_bytes = static_cast<size_t>(out_stride) * static_cast<size_t>(out_h);
#ifdef ESP_PLATFORM
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < data_bytes + 2048)
      return "jpeg: OOM for output buffer";
#endif
    out.data.resize(data_bytes);
    std::fill(out.data.begin(), out.data.end(), uint8_t(0));
  }

  auto err_cur = std::unique_ptr<int16_t[]>(new (std::nothrow) int16_t[out_w + 4]());
  auto err_nxt = std::unique_ptr<int16_t[]>(new (std::nothrow) int16_t[out_w + 4]());
  auto err_nxt2 = std::unique_ptr<int16_t[]>(new (std::nothrow) int16_t[out_w + 4]());
  if (!err_cur || !err_nxt || !err_nxt2)
    return "jpeg: OOM for dither buffers";

  int32_t dc_pred[MAX_COMP] = {};
  int32_t block[64];
  uint8_t pix[64];
  uint32_t mcu_cnt = 0;
  uint32_t total_mcus = static_cast<uint32_t>(mcus_x) * static_cast<uint32_t>(mcus_y);
  int out_y = 0;

  for (int mcu_row = 0; mcu_row < mcus_y; ++mcu_row) {
    std::fill(y_row.get(), y_row.get() + row_w * mcu_h, uint8_t(128));

    for (int mcu_col = 0; mcu_col < mcus_x; ++mcu_col) {
      if (non_interleaved) {
        // Non-interleaved: one block per MCU, always Y (first component).
        int ci = st.scan_order[0];
        const Component& c = st.comp[ci];
        const char* e = decode_block(r, st.dc_huff[c.dc_tbl], st.ac_huff[c.ac_tbl], dc_pred[ci], st.qt[c.qt_idx], block,
                                     st.scan_se, st.scan_al);
        if (e)
          return e;
        idct(block, pix);
        int bx = mcu_col * 8;
        for (int row = 0; row < 8; ++row)
          std::memcpy(y_row.get() + row * row_w + bx, pix + row * 8, 8);
      } else {
        // Interleaved: h_samp × v_samp blocks per component per MCU.
        for (int sci = 0; sci < st.scan_num_comp; ++sci) {
          int ci = st.scan_order[sci];
          const Component& c = st.comp[ci];
          bool is_y = (ci == 0);

          for (int bv = 0; bv < c.v_samp; ++bv) {
            for (int bh = 0; bh < c.h_samp; ++bh) {
              if (is_y) {
                const char* e = decode_block(r, st.dc_huff[c.dc_tbl], st.ac_huff[c.ac_tbl], dc_pred[ci],
                                             st.qt[c.qt_idx], block, st.scan_se, st.scan_al);
                if (e)
                  return e;
                idct(block, pix);
                int bx = mcu_col * mcu_w + bh * 8;
                int by = bv * 8;
                for (int row = 0; row < 8; ++row)
                  std::memcpy(y_row.get() + (by + row) * row_w + bx, pix + row * 8, 8);
              } else {
                const char* e = skip_block(r, st.dc_huff[c.dc_tbl], st.ac_huff[c.ac_tbl], dc_pred[ci], st.scan_se);
                if (e)
                  return e;
              }
            }
          }
        }
      }

      ++mcu_cnt;
      if (st.restart_interval > 0 && mcu_cnt % st.restart_interval == 0 && mcu_cnt < total_mcus) {
        r.consume_restart();
        std::memset(dc_pred, 0, sizeof(dc_pred));
      }
    }

    // Dither this MCU row to 1-bit output
    for (int py = 0; py < mcu_h; ++py) {
      int src_y = mcu_row * mcu_h + py;
      if (src_y >= h || out_y >= out_h)
        break;
      // Emit all output rows that map to this source row (handles upscaling too).
      while (out_y < out_h) {
        int target_src_y = static_cast<int>((static_cast<uint32_t>(out_y) * y_step) >> 16);
        if (target_src_y != src_y)
          break;
        if (sink) {
          uint8_t temp_row[128];  // byte accumulator writes all bytes; no zero-init needed
          dither_row(y_row.get() + py * row_w, x_step, out_w, err_cur.get(), err_nxt.get(), err_nxt2.get(), temp_row);
          sink->emit_row(sink->ctx, static_cast<uint16_t>(out_y), temp_row, static_cast<uint16_t>(out_w));
        } else {
          uint8_t* out_row = out.data.data() + out_y * out_stride;
          dither_row(y_row.get() + py * row_w, x_step, out_w, err_cur.get(), err_nxt.get(), err_nxt2.get(), out_row);
        }
        ++out_y;
        // Rotate three error rows: cur←nxt, nxt←nxt2, nxt2←fresh
        std::swap(err_cur, err_nxt);
        std::swap(err_nxt, err_nxt2);
        std::fill(err_nxt2.get(), err_nxt2.get() + out_w + 4, int16_t(0));
      }
    }
  }

  out.height = static_cast<uint16_t>(out_y);
  return nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ImageError decode_jpeg_from_entry(IZipFile& file, const ZipEntry& entry, uint16_t max_w, uint16_t max_h,
                                  DecodedImage& out, uint8_t* work_buf, size_t work_buf_size, bool scale_to_fill,
                                  ImageRowSink* sink) {
  // If no work_buf provided, allocate one here.
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

  // Heap-allocate JpegState to avoid blowing the stack (~6 KB struct)
  auto st = std::unique_ptr<JpegState>(new (std::nothrow) JpegState());
  if (!st)
    return ImageError::ReadError;

  const char* err = parse_jpeg_header(inp, *st);
  if (err) {
    MR_LOGI("jpeg", "%s", err);
    return ImageError::InvalidData;
  }
  err = validate_tables(*st);
  if (err) {
    MR_LOGI("jpeg", "%s", err);
    return ImageError::InvalidData;
  }

  BitReader r(inp);
#ifdef ESP_PLATFORM
  int64_t _t_jdec = esp_timer_get_time();
#endif
  err = decode_baseline(*st, r, max_w, max_h, out, scale_to_fill, sink);
#ifdef ESP_PLATFORM
  MR_LOGI("jpgd", "decode=%ldms w=%d h=%d", (long)((esp_timer_get_time() - _t_jdec) / 1000), st->width, st->height);
#endif
  if (err) {
    MR_LOGI("jpeg", "%s", err);
    return ImageError::InvalidData;
  }

  return ImageError::Ok;
}

}  // namespace microreader
