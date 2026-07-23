#include "Book.h"

#include <cstdio>
#include <cstring>

#include "../HeapLog.h"

namespace microreader {

EpubError Book::open(const char* path, uint8_t* work_buf, uint8_t* xml_buf, bool parse_css_ncx) {
  close();  // release previous resources
  if (!file_.open(path))
    return EpubError::ZipError;
  file_open_ = true;

  // If caller didn't provide buffers (desktop / tests), allocate here.
  // Two separate allocations so heap fragmentation can't block us: the
  // 45KB work buf and 4KB xml buf fit individually even when no single
  // 49KB block is free.
  static constexpr size_t kWorkBufSize = ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 1024;
  static constexpr size_t kXmlBufSize = 4096;
  std::unique_ptr<uint8_t[]> owned_work;
  std::unique_ptr<uint8_t[]> owned_xml;
  if (!work_buf) {
    owned_work = std::make_unique<uint8_t[]>(kWorkBufSize);
    work_buf = owned_work.get();
  }
  if (!xml_buf) {
    owned_xml = std::make_unique<uint8_t[]>(kXmlBufSize);
    xml_buf = owned_xml.get();
  }

  return epub_.open(file_, work_buf, xml_buf, parse_css_ncx);
}

bool Book::open_zip_only(const char* path) {
  close();
  if (!file_.open(path))
    return false;
  file_open_ = true;
  auto err = epub_.open_zip_only(file_);
  if (err != EpubError::Ok) {
    close();
    return false;
  }
  return true;
}

void Book::close() {
  epub_.close();
  file_.close();
  file_open_ = false;
}

EpubError Book::load_chapter(size_t index, Chapter& out) {
  return epub_.parse_chapter(file_, index, out);
}

EpubError Book::load_chapter_streaming(size_t index, ParagraphSink sink, void* sink_ctx, uint8_t* work_buf,
                                       uint8_t* xml_buf, IdSink id_sink, void* id_sink_ctx) {
  return epub_.parse_chapter_streaming(file_, index, sink, sink_ctx, work_buf, xml_buf, id_sink, id_sink_ctx);
}

ImageError Book::decode_image(uint16_t entry_index, DecodedImage& out, uint16_t max_w, uint16_t max_h,
                              uint8_t* work_buf, size_t work_buf_size) {
  if (!images_enabled)
    return ImageError::UnsupportedFormat;
  if (entry_index >= epub_.zip().entry_count())
    return ImageError::UnsupportedFormat;

  auto& entry = epub_.zip().entry(entry_index);
  return decode_image_from_entry(file_, entry, max_w, max_h, out, work_buf, work_buf_size, /*scale_to_fill=*/true);
}

ZipError Book::extract_entry(uint16_t entry_index, std::vector<uint8_t>& out) {
  if (entry_index >= epub_.zip().entry_count())
    return ZipError::InvalidData;
  auto& entry = epub_.zip().entry(entry_index);
  return epub_.zip().extract(file_, entry, out);
}

bool Book::read_image_size(uint16_t entry_index, uint16_t& w, uint16_t& h, uint8_t* work_buf, size_t work_size) {
  if (entry_index >= epub_.zip().entry_count()) {
    MR_LOGI("book", "read_image_size: entry %u out of range (count=%u)", entry_index,
            (unsigned)epub_.zip().entry_count());
    return false;
  }
  static constexpr size_t kWorkSize = ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 1024;
  std::unique_ptr<uint8_t[]> owned;
  if (!work_buf || work_size < kWorkSize) {
    owned = std::make_unique<uint8_t[]>(kWorkSize);
    work_buf = owned.get();
  }
  ImageSizeStream stream;
  epub_.zip().extract_streaming(
      file_, epub_.zip().entry(entry_index),
      [](const uint8_t* d, size_t n, void* ud) -> bool { return !static_cast<ImageSizeStream*>(ud)->feed(d, n); },
      &stream, work_buf, kWorkSize);
  if (!stream.ok()) {
    MR_LOGI("book", "read_image_size: entry %u stream failed", entry_index);
    return false;
  }
  w = stream.width();
  h = stream.height();
  MR_LOGI("book", "read_image_size: entry %u -> %ux%u", entry_index, w, h);
  return true;
}

bool Book::write_cover_bin(const char* cover_path, int max_w, int max_h,
                            uint8_t* work_buf, size_t work_buf_size) {
  const int idx = epub_.cover_zip_idx();
  if (idx < 0 || idx >= static_cast<int>(epub_.zip().entry_count()))
    return false;
  DecodedImage img;
  // Force images_enabled=true so covers are always generated regardless of
  // the "show reader images" setting.
  auto& entry = epub_.zip().entry(static_cast<uint16_t>(idx));
  const bool was_enabled = images_enabled;
  images_enabled = true;
  auto err = decode_image_from_entry(file_, entry, max_w, max_h, img, work_buf, work_buf_size, /*scale_to_fill=*/false);
  images_enabled = was_enabled;
  if (err != ImageError::Ok || img.data.empty())
    return false;
  FILE* f = std::fopen(cover_path, "wb");
  if (!f)
    return false;
  uint16_t le[2] = {img.width, img.height};
  std::fwrite(le, 2, 2, f);
  std::fwrite(img.data.data(), 1, img.data.size(), f);
  std::fclose(f);
  return true;
}

static std::string epub_stem_(const char* epub_path) {
  const char* p = epub_path;
  const char* slash = nullptr;
  for (const char* c = p; *c; ++c)
    if (*c == '/' || *c == '\\') slash = c;
  const char* name = slash ? slash + 1 : p;
  const char* dot = std::strrchr(name, '.');
  return dot ? std::string(name, static_cast<size_t>(dot - name)) : std::string(name);
}

std::string cover_bin_path(const char* epub_path, const char* data_dir) {
  return std::string(data_dir) + "/cache/" + epub_stem_(epub_path) + "/cover.bin";
}

std::string cover_sleep_bin_path(const char* epub_path, const char* data_dir) {
  return std::string(data_dir) + "/cache/" + epub_stem_(epub_path) + "/cover_sleep.bin";
}

}  // namespace microreader
