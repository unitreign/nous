#include "MrbWriter.h"

#include <cerrno>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static constexpr char kMrbTag[] = "mrb_w";
#endif

namespace microreader {

// ---------------------------------------------------------------------------
// BufferedFileWriter
// ---------------------------------------------------------------------------

bool BufferedFileWriter::open(const char* path) {
  close();
  f_ = fopen(path, "wb");
  if (!f_) {
#ifdef ESP_PLATFORM
    ESP_LOGE(kMrbTag, "bw fopen failed: %s", path);
#endif
    return false;
  }
  // Use unbuffered I/O: our BufferedFileWriter already batches writes into
  // a 4KB buffer, so a second stdio layer just wastes heap and can fail to
  // allocate its buffer on a fragmented ESP32 heap.
  setvbuf(f_, nullptr, _IONBF, 0);
  pos_ = 0;
  used_ = 0;
  return true;
}

void BufferedFileWriter::close() {
  if (f_) {
    flush();
    fclose(f_);
    f_ = nullptr;
  }
  pos_ = 0;
  used_ = 0;
}

bool BufferedFileWriter::flush() {
  if (used_ > 0) {
    if (fwrite(buf_, 1, used_, f_) != used_) {
#ifdef ESP_PLATFORM
      ESP_LOGE(kMrbTag, "flush fwrite failed: used=%u errno=%d (%s)", (unsigned)used_, errno, strerror(errno));
#endif
      return false;
    }
    used_ = 0;
  }
  return true;
}

bool BufferedFileWriter::write(const void* data, size_t size) {
  const uint8_t* src = static_cast<const uint8_t*>(data);
  pos_ += static_cast<uint32_t>(size);
  // Fast path: fits in remaining buffer space.
  if (used_ + size <= kBufSize) {
    std::memcpy(buf_ + used_, src, size);
    used_ += size;
    return true;
  }
  // Flush current buffer.
  if (!flush())
    return false;
  // Large write: bypass buffer entirely.
  if (size >= kBufSize)
    return fwrite(src, 1, size, f_) == size;
  // Small write after flush: start fresh buffer.
  std::memcpy(buf_, src, size);
  used_ = size;
  return true;
}

bool BufferedFileWriter::seek(uint32_t offset) {
  if (!flush())
    return false;
  if (fseek(f_, static_cast<long>(offset), SEEK_SET) != 0)
    return false;
  pos_ = offset;
  return true;
}

// ---------------------------------------------------------------------------
// MrbWriter
// ---------------------------------------------------------------------------

bool MrbWriter::open(const char* path) {
  close();
  if (!bw_.open(path))
    return false;

  // Open a temp file to stream anchor entries during conversion (zero RAM usage).
  std::snprintf(anchor_tmp_path_, sizeof(anchor_tmp_path_), "%s.tmp", path);
  anchor_tmp_ = std::fopen(anchor_tmp_path_, "w+b");
#ifdef ESP_PLATFORM
  if (!anchor_tmp_)
    ESP_LOGW(kMrbTag, "anchor_tmp fopen failed: %s", anchor_tmp_path_);
#endif
  anchor_count_ = 0;

  // Open a single temp file for all paragraph descriptors across the whole book.
  // Reused across chapters; chapter_desc_start_ tracks where each chapter begins.
  std::snprintf(desc_tmp_path_, sizeof(desc_tmp_path_), "%s.desc", path);
  desc_tmp_ = std::fopen(desc_tmp_path_, "w+b");
  chapter_desc_start_ = 0;
  if (!desc_tmp_) {
#ifdef ESP_PLATFORM
    ESP_LOGE(kMrbTag, "desc_tmp fopen failed: %s", desc_tmp_path_);
#endif
    close();
    return false;
  }

  // Write placeholder header (will be fixed up in finish()).
  MrbHeader hdr{};
  std::memcpy(hdr.magic, kMrbMagic, 4);
  hdr.version = kMrbVersion;
  if (!write_bytes(&hdr, sizeof(hdr))) {
    close();
    return false;
  }
  return true;
}

void MrbWriter::close() {
  bw_.close();
  if (anchor_tmp_) {
    std::fclose(anchor_tmp_);
    anchor_tmp_ = nullptr;
    if (anchor_tmp_path_[0])
      std::remove(anchor_tmp_path_);
    anchor_tmp_path_[0] = '\0';
  }
  if (desc_tmp_) {
    std::fclose(desc_tmp_);
    desc_tmp_ = nullptr;
    if (desc_tmp_path_[0])
      std::remove(desc_tmp_path_);
    desc_tmp_path_[0] = '\0';
  }
  anchor_count_ = 0;
  chapter_desc_start_ = 0;
  paragraph_count_ = 0;
  chapters_.clear();
  images_.clear();
  in_chapter_ = false;
  chapter_para_count_ = 0;
  chapter_char_count_ = 0;
}

void MrbWriter::begin_chapter() {
  chapter_para_count_ = 0;
  chapter_char_count_ = 0;
  // Record where this chapter's descriptors start in desc_tmp_.
  if (desc_tmp_) {
    std::fseek(desc_tmp_, 0, SEEK_END);
    chapter_desc_start_ = static_cast<uint32_t>(std::ftell(desc_tmp_));
  }
  in_chapter_ = true;
}

void MrbWriter::end_chapter() {
  if (!in_chapter_)
    return;

  // Write the descriptor table by reading back from desc_tmp_.
  uint32_t table_offset = bw_.tell();
  if (desc_tmp_ && chapter_para_count_ > 0) {
    std::fseek(desc_tmp_, static_cast<long>(chapter_desc_start_), SEEK_SET);
    uint32_t remaining = chapter_para_count_ * 8u;
    uint8_t copy_buf[128];
    while (remaining > 0) {
      size_t want = remaining < sizeof(copy_buf) ? remaining : sizeof(copy_buf);
      size_t n = std::fread(copy_buf, 1, want, desc_tmp_);
      if (n == 0)
        break;
      write_bytes(copy_buf, n);
      remaining -= static_cast<uint32_t>(n);
    }
  }

  MrbChapterEntry entry{};
  entry.para_table_offset = table_offset;
  entry.reserved = 0;
  entry.paragraph_count = chapter_para_count_;
  entry.reserved1 = 0;
  entry.char_count = chapter_char_count_;
  chapters_.push_back(entry);
  in_chapter_ = false;
}

bool MrbWriter::write_paragraph(const Paragraph& para) {
  if (!bw_.is_open())
    return false;

  // Text paragraphs are handled entirely by write_text_paragraph (descriptor + counters included).
  if (para.type == ParagraphType::Text) {
    uint16_t spacing = para.spacing_before.value_or(kMrbSpacingDefault);
    return write_text_paragraph(para.text, spacing, para.text.runs.data(), para.text.runs.size());
  }

  // Record descriptor for non-text paragraphs: append {file_offset, char_offset} to desc_tmp_.
  if (desc_tmp_) {
    uint8_t desc[8];
    mrb_write_u32(desc, bw_.tell());
    mrb_write_u32(desc + 4, chapter_char_count_);
    std::fwrite(desc, 1, 8, desc_tmp_);
  }

  // Serialize and write: [type(1)][data_size(4)][data...] — no link header.
  switch (para.type) {
    case ParagraphType::Text:
      break;  // unreachable — handled above
    case ParagraphType::Image: {
      uint8_t buf[9];
      buf[0] = kMrbParaImage;
      mrb_write_u32(buf + 1, 4);
      mrb_write_u16(buf + 5, para.image.key);
      mrb_write_u16(buf + 7, para.spacing_before.value_or(kMrbSpacingDefault));
      if (!write_bytes(buf, 9))
        return false;
      break;
    }
    case ParagraphType::Hr: {
      uint8_t buf[8];
      buf[0] = kMrbParaHr;
      mrb_write_u32(buf + 1, 3);
      mrb_write_u16(buf + 5, para.spacing_before.value_or(kMrbSpacingDefault));
      buf[7] = para.hr_width_pct.value_or(kMrbHrWidthDefault);
      if (!write_bytes(buf, 8))
        return false;
      break;
    }
    case ParagraphType::PageBreak: {
      uint8_t buf[5];
      buf[0] = kMrbParaPageBreak;
      mrb_write_u32(buf + 1, 0);
      if (!write_bytes(buf, 5))
        return false;
      break;
    }
  }

  ++chapter_para_count_;
  ++paragraph_count_;
  return true;
}

uint16_t MrbWriter::add_image_ref(uint32_t local_header_offset, uint16_t width, uint16_t height) {
  uint16_t idx = static_cast<uint16_t>(images_.size());
  MrbImageRef ref{};
  ref.local_header_offset = local_header_offset;
  ref.width = width;
  ref.height = height;
  images_.push_back(ref);
  return idx;
}

void MrbWriter::update_image_size(uint16_t idx, uint16_t width, uint16_t height) {
  if (idx < images_.size()) {
    images_[idx].width = width;
    images_[idx].height = height;
  }
}

void MrbWriter::add_anchor(uint16_t chapter_idx, uint16_t para_index, const char* id, size_t id_len) {
  if (id_len == 0 || id_len > 255 || !anchor_tmp_)
    return;
  uint8_t hdr_buf[5];
  mrb_write_u16(hdr_buf, chapter_idx);
  mrb_write_u16(hdr_buf + 2, para_index);
  hdr_buf[4] = static_cast<uint8_t>(id_len);
  if (std::fwrite(hdr_buf, 1, 5, anchor_tmp_) == 5 && std::fwrite(id, 1, id_len, anchor_tmp_) == id_len)
    ++anchor_count_;
}

bool MrbWriter::finish(const EpubMetadata& meta, const TableOfContents& toc,
                       const std::vector<std::string>& spine_files) {
  if (!bw_.is_open())
    return false;

  // Close any open chapter.
  if (in_chapter_)
    end_chapter();

  // --- Write chapter table (16 bytes each) ---
  uint32_t chapter_offset = bw_.tell();
  for (const auto& ch : chapters_) {
    uint8_t buf[16];
    mrb_write_u32(buf, ch.para_table_offset);
    mrb_write_u32(buf + 4, ch.reserved);
    mrb_write_u16(buf + 8, ch.paragraph_count);
    mrb_write_u16(buf + 10, 0);
    mrb_write_u32(buf + 12, ch.char_count);
    if (!write_bytes(buf, 16))
      return false;
  }

  // --- Write image ref table ---
  uint32_t image_offset = bw_.tell();
  for (const auto& img : images_) {
    uint8_t buf[8];
    mrb_write_u32(buf, img.local_header_offset);
    mrb_write_u16(buf + 4, img.width);
    mrb_write_u16(buf + 6, img.height);
    if (!write_bytes(buf, 8))
      return false;
  }

  // --- Write metadata blob ---
  uint32_t meta_offset = bw_.tell();
  write_string(meta.title);
  write_string(meta.author.value_or(""));
  write_string(meta.language.value_or(""));

  // --- Write TOC ---
  uint16_t toc_count = static_cast<uint16_t>(toc.entries.size());
  uint8_t toc_hdr[2];
  mrb_write_u16(toc_hdr, toc_count);
  write_bytes(toc_hdr, 2);
  for (const auto& entry : toc.entries) {
    auto lbl = toc.label_of(entry);
    write_string(std::string(lbl));
    uint8_t buf[5];
    mrb_write_u16(buf, entry.file_idx);
    buf[2] = entry.depth;
    mrb_write_u16(buf + 3, entry.para_index);
    write_bytes(buf, 5);
  }

  // --- Write spine file table ---
  // Allows runtime resolution of href filenames → chapter indices.
  {
    uint16_t spine_count = static_cast<uint16_t>(spine_files.size());
    uint8_t sc_buf[2];
    mrb_write_u16(sc_buf, spine_count);
    write_bytes(sc_buf, 2);
    for (const auto& name : spine_files)
      write_string(name);
  }

  // --- Write anchor table ---
  // Format: [count:u16] then count × [chapter_idx:u16][para_idx:u16][id_len:u8][id_bytes]
  uint32_t anchor_offset = bw_.tell();
  {
    uint8_t ac_buf[2];
    mrb_write_u16(ac_buf, static_cast<uint16_t>(anchor_count_));
    write_bytes(ac_buf, 2);
    // Copy streamed anchor entries from temp file.
    if (anchor_tmp_ && anchor_count_ > 0) {
      std::rewind(anchor_tmp_);
      uint8_t copy_buf[128];
      size_t n;
      while ((n = std::fread(copy_buf, 1, sizeof(copy_buf), anchor_tmp_)) > 0)
        write_bytes(copy_buf, n);
    }
  }

  // --- Fix up header ---
  MrbHeader hdr{};
  std::memcpy(hdr.magic, kMrbMagic, 4);
  hdr.version = kMrbVersion;
  hdr.flags = 0;
  hdr.paragraph_count = paragraph_count_;
  hdr.chapter_count = static_cast<uint16_t>(chapters_.size());
  hdr.image_count = static_cast<uint16_t>(images_.size());
  hdr.anchor_offset = anchor_offset;
  hdr.chapter_offset = chapter_offset;
  hdr.image_offset = image_offset;
  hdr.meta_offset = meta_offset;

  bw_.seek(0);
  if (!write_bytes(&hdr, sizeof(hdr)))
    return false;
  bw_.close();

  return true;
}

// ---------------------------------------------------------------------------
// write_text_paragraph  (streams directly to bw_ — no intermediate buffer)
// ---------------------------------------------------------------------------

bool MrbWriter::write_text_paragraph(const TextParagraph& meta, uint16_t spacing, const Run* runs, size_t run_count) {
  if (!bw_.is_open())
    return false;

  // Record descriptor.
  if (desc_tmp_) {
    uint8_t desc[8];
    mrb_write_u32(desc, bw_.tell());
    mrb_write_u32(desc + 4, chapter_char_count_);
    std::fwrite(desc, 1, 8, desc_tmp_);
  }

  // Count chars.
  for (size_t i = 0; i < run_count; ++i)
    chapter_char_count_ += static_cast<uint32_t>(runs[i].text.size());

  // Compute body size (pure arithmetic, no allocation).
  static constexpr size_t kBodyHdrSize = 18;  // alignment(1)+indent(2)+ml(2)+mr(2)+spacing(2)+lh(1)+img(6)+runcount(2)
  static constexpr size_t kRunHdrSize = 12;   // style(1)+size(1)+valign(1)+flags(1)+ml(2)+mr(2)+textlen(4)
  size_t body_size = kBodyHdrSize;
  for (size_t i = 0; i < run_count; ++i) {
    body_size += kRunHdrSize + runs[i].text.size();
    if (!runs[i].href.empty())
      body_size += 2 + runs[i].href.size();
  }

  // Write outer header: [type(1)][body_size(4)]
  uint8_t outer[5];
  outer[0] = kMrbParaText;
  mrb_write_u32(outer + 1, static_cast<uint32_t>(body_size));
  if (!write_bytes(outer, 5))
    return false;

  // Write paragraph body header (18 bytes) from stack.
  uint8_t hdr[18];
  uint8_t* p = hdr;
  *p++ = meta.alignment.has_value() ? static_cast<uint8_t>(*meta.alignment) : kMrbAlignDefault;
  mrb_write_i16(p, meta.indent.value_or(kMrbIndentNone));
  p += 2;
  mrb_write_u16(p, 0);
  p += 2;  // margin_left placeholder
  mrb_write_u16(p, 0);
  p += 2;  // margin_right placeholder
  mrb_write_u16(p, spacing);
  p += 2;
  *p++ = meta.line_height_pct;
  if (meta.inline_image.has_value()) {
    mrb_write_u16(p, meta.inline_image->key);
    p += 2;
    mrb_write_u16(p, meta.inline_image->attr_width);
    p += 2;
    mrb_write_u16(p, meta.inline_image->attr_height);
    p += 2;
  } else {
    mrb_write_u16(p, kMrbNoImage);
    p += 2;
    mrb_write_u16(p, 0);
    p += 2;
    mrb_write_u16(p, 0);
    p += 2;
  }
  mrb_write_u16(p, static_cast<uint16_t>(run_count));
  p += 2;
  if (!write_bytes(hdr, 18))
    return false;

  // Write each run using a 12-byte stack header.
  for (size_t i = 0; i < run_count; ++i) {
    const Run& run = runs[i];
    uint8_t rhdr[12];
    rhdr[0] = static_cast<uint8_t>(run.style);
    rhdr[1] = run.size_pct;
    rhdr[2] = static_cast<uint8_t>(run.vertical_align);
    uint8_t flags = run.breaking ? 0x01 : 0x00;
    if (!run.href.empty())
      flags |= 0x02;
    rhdr[3] = flags;
    mrb_write_u16(rhdr + 4, run.margin_left);
    mrb_write_u16(rhdr + 6, run.margin_right);
    mrb_write_u32(rhdr + 8, static_cast<uint32_t>(run.text.size()));
    if (!write_bytes(rhdr, 12))
      return false;
    if (!run.text.empty() && !write_bytes(run.text.data(), run.text.size()))
      return false;
    if (!run.href.empty()) {
      uint8_t hlen[2];
      mrb_write_u16(hlen, static_cast<uint16_t>(run.href.size()));
      if (!write_bytes(hlen, 2))
        return false;
      if (!write_bytes(run.href.data(), run.href.size()))
        return false;
    }
  }

  ++chapter_para_count_;
  ++paragraph_count_;
  return true;
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

bool MrbWriter::write_bytes(const void* data, size_t size) {
  return bw_.write(data, size);
}

bool MrbWriter::write_string(const std::string& s) {
  uint8_t len_buf[2];
  mrb_write_u16(len_buf, static_cast<uint16_t>(s.size()));
  if (!write_bytes(len_buf, 2))
    return false;
  if (!s.empty() && !write_bytes(s.data(), s.size()))
    return false;
  return true;
}

}  // namespace microreader
