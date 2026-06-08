#include "MrbReader.h"

#include <cstring>

namespace microreader {

bool MrbReader::open(const char* path) {
  close();
  f_ = fopen(path, "rb");
  if (!f_)
    return false;

  // Read header
  if (!read_bytes(&header_, sizeof(header_))) {
    close();
    return false;
  }
  if (std::memcmp(header_.magic, kMrbMagic, 4) != 0 || header_.version != kMrbVersion) {
    close();
    return false;
  }

  // Read chapter table (16 bytes per entry in v2)
  chapters_.resize(header_.chapter_count);
  if (header_.chapter_count > 0) {
    fseek(f_, static_cast<long>(header_.chapter_offset), SEEK_SET);
    for (uint16_t i = 0; i < header_.chapter_count; ++i) {
      uint8_t buf[16];
      if (!read_bytes(buf, 16)) {
        close();
        return false;
      }
      chapters_[i].para_table_offset = mrb_read_u32(buf);
      chapters_[i].reserved = mrb_read_u32(buf + 4);
      chapters_[i].paragraph_count = mrb_read_u16(buf + 8);
      chapters_[i].reserved1 = mrb_read_u16(buf + 10);
      chapters_[i].char_count = mrb_read_u32(buf + 12);
    }
  }

  // Read image ref table
  images_.resize(header_.image_count);
  if (header_.image_count > 0) {
    fseek(f_, static_cast<long>(header_.image_offset), SEEK_SET);
    for (uint16_t i = 0; i < header_.image_count; ++i) {
      uint8_t buf[8];
      if (!read_bytes(buf, 8)) {
        close();
        return false;
      }
      images_[i].local_header_offset = mrb_read_u32(buf);
      images_[i].width = mrb_read_u16(buf + 4);
      images_[i].height = mrb_read_u16(buf + 6);
    }
  }

  // Read metadata
  fseek(f_, static_cast<long>(header_.meta_offset), SEEK_SET);
  metadata_.title = read_string();
  std::string author = read_string();
  if (!author.empty())
    metadata_.author = std::move(author);
  std::string lang = read_string();
  if (!lang.empty())
    metadata_.language = std::move(lang);

  // Read TOC
  uint8_t toc_hdr[2];
  if (read_bytes(toc_hdr, 2)) {
    uint16_t toc_count = mrb_read_u16(toc_hdr);
    toc_.entries.reserve(toc_count);
    for (uint16_t i = 0; i < toc_count; ++i) {
      std::string label = read_string();
      uint8_t buf[5] = {};
      read_bytes(buf, 5);
      uint16_t file_idx = mrb_read_u16(buf);
      uint8_t depth = buf[2];
      uint16_t para_index = mrb_read_u16(buf + 3);
      toc_.add_entry(label, file_idx, depth, {}, para_index);
    }
  }

  // v9+: Read spine file table (chapter filename → index mapping for href resolution).
  {
    uint8_t sc_buf[2];
    if (read_bytes(sc_buf, 2)) {
      uint16_t spine_count = mrb_read_u16(sc_buf);
      spine_files_.resize(spine_count);
      for (uint16_t i = 0; i < spine_count; ++i)
        spine_files_[i] = read_string();
    }
  }

  // v10+: Read anchor table header (count only; entries are seeked on demand).
  anchor_offset_ = header_.anchor_offset;
  if (anchor_offset_ != 0) {
    uint8_t ac_buf[2];
    if (fseek(f_, static_cast<long>(anchor_offset_), SEEK_SET) == 0 && read_bytes(ac_buf, 2))
      anchor_count_ = mrb_read_u16(ac_buf);
  }

  return true;
}

void MrbReader::close() {
  if (f_) {
    fclose(f_);
    f_ = nullptr;
  }
  chapters_.clear();
  chapters_.shrink_to_fit();
  images_.clear();
  images_.shrink_to_fit();
  header_ = {};
  metadata_ = {};
  toc_ = {};
  spine_files_.clear();
  anchor_offset_ = 0;
  anchor_count_ = 0;
}

uint32_t MrbReader::chapter_para_table_offset(uint16_t chapter_idx) const {
  if (chapter_idx >= chapters_.size())
    return 0;
  return chapters_[chapter_idx].para_table_offset;
}

uint16_t MrbReader::chapter_paragraph_count(uint16_t chapter_idx) const {
  if (chapter_idx >= chapters_.size())
    return 0;
  return chapters_[chapter_idx].paragraph_count;
}

uint32_t MrbReader::chapter_char_count(uint16_t chapter_idx) const {
  if (chapter_idx >= chapters_.size())
    return 0;
  return chapters_[chapter_idx].char_count;
}

uint64_t MrbReader::total_char_count() const {
  uint64_t total = 0;
  for (const auto& ch : chapters_)
    total += ch.char_count;
  return total;
}

bool MrbReader::find_anchor(uint16_t chapter_idx, const char* fragment, size_t frag_len, uint16_t& para_idx) const {
  if (!f_ || anchor_count_ == 0 || frag_len == 0 || frag_len > 255)
    return false;

  // Anchor table layout: [count:u16] then entries:
  //   [chapter_idx:u16][para_idx:u16][id_len:u8][id_bytes]
  // anchor_offset_ points to the count field; entries start 2 bytes later.
  if (fseek(f_, static_cast<long>(anchor_offset_ + 2), SEEK_SET) != 0)
    return false;

  char id_buf[256];
  for (uint16_t i = 0; i < anchor_count_; ++i) {
    uint8_t hdr_buf[5];
    if (fread(hdr_buf, 1, 5, f_) != 5)
      return false;
    uint16_t ch_idx = mrb_read_u16(hdr_buf);
    uint16_t para = mrb_read_u16(hdr_buf + 2);
    uint8_t id_len = hdr_buf[4];
    if (fread(id_buf, 1, id_len, f_) != id_len)
      return false;
    if (ch_idx == chapter_idx && id_len == static_cast<uint8_t>(frag_len) &&
        std::memcmp(id_buf, fragment, frag_len) == 0) {
      para_idx = para;
      return true;
    }
  }
  return false;
}

MrbReader::LoadResult MrbReader::load_paragraph(uint32_t file_offset, Paragraph& out) {
  LoadResult result;
  if (!f_ || file_offset == 0)
    return result;

  fseek(f_, static_cast<long>(file_offset), SEEK_SET);

  // Read type + data_size (5 bytes). No link header in v8+.
  uint8_t hdr[5];
  if (!read_bytes(hdr, 5))
    return result;

  uint8_t type = hdr[0];
  uint32_t data_size = mrb_read_u32(hdr + 1);

  // Read body
  std::vector<uint8_t> body(data_size);
  if (data_size > 0 && !read_bytes(body.data(), data_size))
    return result;

  switch (type) {
    case kMrbParaText:
      if (deserialize_text(body.data(), body.size(), out))
        result.ok = true;
      return result;

    case kMrbParaImage: {
      out = Paragraph{};
      out.type = ParagraphType::Image;
      if (data_size >= 4) {
        uint16_t key = mrb_read_u16(body.data());
        // Look up dimensions from image ref table
        uint16_t w = 0, h = 0;
        if (key < images_.size()) {
          w = images_[key].width;
          h = images_[key].height;
        }
        out.image = ImageRef{key, w, h};
        uint16_t sp = mrb_read_u16(body.data() + 2);
        if (sp != kMrbSpacingDefault)
          out.spacing_before = sp;
      }
      result.ok = true;
      return result;
    }

    case kMrbParaHr: {
      out = Paragraph::make_hr();
      if (data_size >= 2) {
        uint16_t sp = mrb_read_u16(body.data());
        if (sp != kMrbSpacingDefault)
          out.spacing_before = sp;
      }
      if (data_size >= 3) {
        uint8_t w = mrb_read_u8(body.data() + 2);
        if (w != kMrbHrWidthDefault)
          out.hr_width_pct = w;
      }
      result.ok = true;
      return result;
    }

    case kMrbParaPageBreak:
      out = Paragraph::make_page_break();
      result.ok = true;
      return result;

    default:
      return result;
  }
}

// ---------------------------------------------------------------------------
// Text paragraph deserialization
// ---------------------------------------------------------------------------

bool MrbReader::deserialize_text(const uint8_t* data, size_t size, Paragraph& out) {
  out = Paragraph{};
  out.type = ParagraphType::Text;

  if (size < 18)  // minimum: header fields + run_count
    return false;

  size_t pos = 0;

  // alignment
  uint8_t align_val = data[pos++];
  if (align_val != kMrbAlignDefault)
    out.text.alignment = static_cast<Alignment>(align_val);

  // indent
  int16_t indent_val = mrb_read_i16(data + pos);
  pos += 2;
  if (indent_val != kMrbIndentNone)
    out.text.indent = indent_val;

  // margin_left, margin_right (paragraph-level, currently unused placeholders)
  pos += 2;  // skip margin_left
  pos += 2;  // skip margin_right

  // spacing_before
  uint16_t spacing = mrb_read_u16(data + pos);
  pos += 2;
  if (spacing != kMrbSpacingDefault)
    out.spacing_before = spacing;

  // line_height_pct
  out.text.line_height_pct = data[pos++];

  // inline image
  uint16_t img_key = mrb_read_u16(data + pos);
  pos += 2;
  uint16_t img_w = mrb_read_u16(data + pos);
  pos += 2;
  uint16_t img_h = mrb_read_u16(data + pos);
  pos += 2;
  if (img_key != kMrbNoImage) {
    // Fall back to image ref table if inline dimensions are unknown.
    if ((img_w == 0 || img_h == 0) && img_key < images_.size()) {
      img_w = images_[img_key].width;
      img_h = images_[img_key].height;
    }
    out.text.inline_image = ImageRef{img_key, img_w, img_h};
  }

  // run count
  if (pos + 2 > size)
    return false;
  uint16_t run_count = mrb_read_u16(data + pos);
  pos += 2;

  out.text.runs.resize(run_count);
  for (uint16_t i = 0; i < run_count; ++i) {
    if (pos + 12 > size)
      return false;

    Run& run = out.text.runs[i];
    run.style = static_cast<FontStyle>(data[pos++]);
    run.size_pct = data[pos++];
    run.vertical_align = static_cast<VerticalAlign>(data[pos++]);
    uint8_t flags = data[pos++];
    run.breaking = (flags & 0x01) != 0;

    run.margin_left = mrb_read_u16(data + pos);
    pos += 2;
    run.margin_right = mrb_read_u16(data + pos);
    pos += 2;

    uint32_t text_len = mrb_read_u32(data + pos);
    pos += 4;

    if (pos + text_len > size)
      return false;
    run.text.assign(reinterpret_cast<const char*>(data + pos), text_len);
    pos += text_len;

    // v9+: href string follows text bytes when flags bit 0x02 is set.
    if (flags & 0x02) {
      if (pos + 2 > size)
        return false;
      uint16_t href_len = mrb_read_u16(data + pos);
      pos += 2;
      if (pos + href_len > size)
        return false;
      run.href.assign(reinterpret_cast<const char*>(data + pos), href_len);
      pos += href_len;
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

bool MrbReader::read_bytes(void* buf, size_t size) {
  return fread(buf, 1, size, f_) == size;
}

bool MrbReader::read_at(uint32_t offset, void* buf, size_t size) {
  fseek(f_, static_cast<long>(offset), SEEK_SET);
  return read_bytes(buf, size);
}

std::string MrbReader::read_string() {
  uint8_t len_buf[2];
  if (!read_bytes(len_buf, 2))
    return {};
  uint16_t len = mrb_read_u16(len_buf);
  if (len == 0)
    return {};
  std::string s(len, '\0');
  if (!read_bytes(s.data(), len))
    return {};
  return s;
}

}  // namespace microreader
