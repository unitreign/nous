#include "MrbConverter.h"

#include "../../display/DrawBuffer.h"
#include "../EpubParser.h"
#include "../ZipReader.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace microreader {

// Shared image-key remapping used by both converter paths.
namespace {
struct ImageMapping {
  uint16_t zip_key;
  uint16_t mrb_idx;
};

uint16_t get_or_add_image(MrbWriter& writer, std::vector<ImageMapping>& image_map, uint16_t zip_key,
                          uint32_t local_offset, uint16_t w, uint16_t h) {
  bool caller_has_size = (w != 0 || h != 0);
  for (const auto& m : image_map) {
    if (m.zip_key == zip_key && writer.image_size_known(m.mrb_idx) == caller_has_size)
      return m.mrb_idx;
  }
  uint16_t idx = writer.add_image_ref(local_offset, w, h);
  image_map.push_back({zip_key, idx});
  return idx;
}

void remap_paragraph_images(Paragraph& para, MrbWriter& writer, std::vector<ImageMapping>& image_map,
                            const ZipReader& zip) {
  if (para.type == ParagraphType::Image) {
    uint32_t offset = zip.entry(para.image.key).local_header_offset;
    para.image.key =
        get_or_add_image(writer, image_map, para.image.key, offset, para.image.attr_width, para.image.attr_height);
  }
  if (para.type == ParagraphType::Text && para.text.inline_image.has_value()) {
    auto& img = *para.text.inline_image;
    uint32_t offset = zip.entry(img.key).local_header_offset;
    img.key = get_or_add_image(writer, image_map, img.key, offset, img.attr_width, img.attr_height);
  }
}

// Split a large text paragraph into chunks to keep the serialized body small.
// Budget is the estimated on-disk size: 18 (para header) + runs×12 (run headers) + text bytes.
// Keeping them small avoids large contiguous heap allocations at read time on ESP32-C3.
// Annotated books can have thousands of tiny/empty inline spans — the 12-byte run header
// dominates even when total text bytes are small.
static constexpr size_t kMaxSerializedBody = 8192;  // max estimated serialized size per chunk
static constexpr size_t kParaHeaderBytes = 18;
static constexpr size_t kRunHeaderBytes = 12;

// Returns estimated serialized size for a text paragraph.
static size_t estimated_body_size(const TextParagraph& text) {
  size_t sz = kParaHeaderBytes;
  for (const auto& run : text.runs)
    sz += kRunHeaderBytes + run.text.size() + (run.href.empty() ? 0 : 2 + run.href.size());
  return sz;
}

bool write_split_paragraph(MrbWriter& writer, Paragraph& para) {
  if (para.type != ParagraphType::Text)
    return writer.write_paragraph(para);

  const size_t est = estimated_body_size(para.text);

  if (est <= kMaxSerializedBody)
    return writer.write_paragraph(para);

#ifdef ESP_PLATFORM
  ESP_LOGI("split", "splitting para: runs=%u est=%u", (unsigned)para.text.runs.size(), (unsigned)est);
#endif

  // Write the paragraph as a series of chunks, each within kMaxSerializedBody bytes.
  // We track slice indices into para.text.runs — no intermediate vector is allocated.
  // Only the first chunk gets indent, spacing_before, and inline_image.
  const auto& runs = para.text.runs;
  const size_t total_runs = runs.size();

  // Shared metadata for all chunks (no runs vector — passed as pointer slices).
  TextParagraph meta;
  meta.alignment = para.text.alignment;
  meta.line_height_pct = para.text.line_height_pct;
  meta.indent = para.text.indent;
  meta.inline_image = para.text.inline_image;
  const uint16_t first_spacing = para.spacing_before.value_or(kMrbSpacingDefault);

  size_t chunk_start = 0;
  size_t chunk_est = kParaHeaderBytes;
  bool is_first = true;
  bool ok = true;

  auto flush_range = [&](size_t start, size_t end) {
    if (start == end)
      return;
    if (!writer.write_text_paragraph(meta, is_first ? first_spacing : kMrbSpacingDefault, runs.data() + start,
                                     end - start))
      ok = false;
    if (is_first) {
      is_first = false;
      meta.indent.reset();
      meta.inline_image.reset();
    }
    chunk_est = kParaHeaderBytes;
    chunk_start = end;
  };

  for (size_t i = 0; i < total_runs; ++i) {
    const Run& run = runs[i];
    const size_t run_cost = kRunHeaderBytes + run.text.size() + (run.href.empty() ? 0 : 2 + run.href.size());

    // If a single run exceeds the budget on its own, flush whatever we have first,
    // then emit it as its own chunk (rare — would require >8KB of text in one run).
    if (run_cost >= kMaxSerializedBody) {
      flush_range(chunk_start, i);
      chunk_est += run_cost;
      continue;
    }

    // Would this run overflow the current chunk? Flush first.
    if (i > chunk_start && chunk_est + run_cost > kMaxSerializedBody)
      flush_range(chunk_start, i);

    chunk_est += run_cost;
  }

  flush_range(chunk_start, total_runs);
  return ok;
}

}  // namespace

bool convert_epub_to_mrb_streaming(Book& book, const char* output_path, uint8_t* work_buf, uint8_t* xml_buf,
                                   std::function<void(int, int)> progress_cb) {
  MrbWriter writer;
  if (!writer.open(output_path)) {
#ifdef ESP_PLATFORM
    ESP_LOGE("mrb", "writer.open failed: %s", output_path);
#endif
    return false;
  }

  // On ESP32, caller always passes pre-allocated buffers. On desktop (tests etc.)
  // allocate here so the leaf parse_chapter_streaming never has to.
  static constexpr size_t kWorkBufSize = ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 2048;
  static constexpr size_t kXmlBufSize = 16384;
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

  std::vector<ImageMapping> image_map;
  const auto& zip = book.epub().zip();

#ifdef ESP_PLATFORM
  int64_t total_start = esp_timer_get_time();
#endif

  // --- Fragment → para_index resolution setup ---
  // Build a working copy of the TOC early so we can resolve fragment anchors.
  struct FragmentNeed {
    uint16_t zip_file_idx;  // zip entry index of the XHTML file (raw, before spine remapping)
    std::string fragment;   // the id value to locate
    size_t toc_entry_idx;   // index into toc_work.entries to fill in
  };
  // Use a reference — moving the TOC would invalidate StringRef::pool pointers.
  TableOfContents& toc_work = book.toc();

  std::vector<FragmentNeed> fragment_needs;
  fragment_needs.reserve(toc_work.entries.size());
  for (size_t i = 0; i < toc_work.entries.size(); ++i) {
    auto frag = toc_work.entries[i].fragment.to_string(toc_work.pool);
    if (!frag.empty()) {
      fragment_needs.push_back({toc_work.entries[i].file_idx, std::string(frag), i});
    }
  }

  // Context for the streaming paragraph + ID sinks.
  struct SinkCtx {
    MrbWriter* writer;
    std::vector<ImageMapping>* image_map;
    const ZipReader* zip;
    bool error;
    // Fragment resolution (nullptr if no fragments need resolving)
    std::vector<FragmentNeed>* fragment_needs;
    TableOfContents* toc_work;
    uint16_t current_zip_file_idx;
    uint16_t current_chapter_idx;
    // (anchors are written directly to the MrbWriter as they arrive)
#ifdef ESP_PLATFORM
    int64_t write_us;  // time in write_paragraph (MRB I/O)
    size_t para_count;
#endif
  };
  SinkCtx ctx{};
  ctx.writer = &writer;
  ctx.image_map = &image_map;
  ctx.zip = &zip;
  ctx.error = false;
  ctx.fragment_needs = &fragment_needs;
  ctx.toc_work = &toc_work;
  ctx.current_zip_file_idx = 0;
  ctx.current_chapter_idx = 0;
#ifdef ESP_PLATFORM
  ctx.write_us = 0;
  ctx.para_count = 0;
  unsigned total_paras = 0;
  long slowest_ms = 0;
  int slowest_ci = -1;
  int64_t last_log_us = esp_timer_get_time();
#endif

  // ID sink: always active.
  // Resolves TOC fragment anchors AND collects all id→para mappings for runtime link navigation.
  IdSink id_sink = [](void* raw_ctx, const char* id_p, size_t id_len, uint32_t para_idx) {
    auto& c = *static_cast<SinkCtx*>(raw_ctx);
    // TOC fragment resolution.
    for (auto& need : *c.fragment_needs) {
      if (need.zip_file_idx == c.current_zip_file_idx && need.fragment.size() == id_len &&
          std::memcmp(need.fragment.data(), id_p, id_len) == 0) {
        auto& entry = c.toc_work->entries[need.toc_entry_idx];
        if (entry.para_index == 0)  // only record first match per entry
          entry.para_index = static_cast<uint16_t>(para_idx < 0xFFFFu ? para_idx : 0xFFFFu);
      }
    }
    // Anchor collection: write directly to MRB to avoid buffering all anchors in RAM.
    if (id_len > 0 && id_len <= 255 && para_idx < 0xFFFFu)
      c.writer->add_anchor(c.current_chapter_idx, static_cast<uint16_t>(para_idx), id_p, id_len);
  };

  // Non-text paragraph sink: remap image keys, split large text paragraphs, and write to MRB.
  auto sink = [](void* raw_ctx, Paragraph&& para) {
    auto& c = *static_cast<SinkCtx*>(raw_ctx);
    if (c.error)
      return;

    remap_paragraph_images(para, *c.writer, *c.image_map, *c.zip);

#ifdef ESP_PLATFORM
    int64_t t0 = esp_timer_get_time();
#endif
    bool write_ok = write_split_paragraph(*c.writer, para);
#ifdef ESP_PLATFORM
    c.write_us += esp_timer_get_time() - t0;
    c.para_count++;
#endif
    if (!write_ok) {
#ifdef ESP_PLATFORM
      ESP_LOGE("mrb", "write_split_paragraph failed at ch %u", (unsigned)c.current_chapter_idx);
#endif
      c.error = true;
    }
  };

  for (size_t ci = 0; ci < book.chapter_count(); ++ci) {
    writer.begin_chapter();

#ifdef ESP_PLATFORM
    int64_t ch_start = esp_timer_get_time();
#endif

    ctx.current_zip_file_idx = static_cast<uint16_t>(book.epub().spine()[ci].file_idx);
    ctx.current_chapter_idx = static_cast<uint16_t>(ci);
    book.load_chapter_streaming(ci, sink, &ctx, work_buf, xml_buf, id_sink, &ctx);
    if (ctx.error) {
#ifdef ESP_PLATFORM
      ESP_LOGE("mrb", "ctx.error after ch %u", (unsigned)ci);
#endif
      return false;
    }

    writer.end_chapter();

    if (progress_cb)
      progress_cb(static_cast<int>(ci + 1), static_cast<int>(book.chapter_count()));

#ifdef ESP_PLATFORM
    long ch_ms = (long)((esp_timer_get_time() - ch_start) / 1000);
    long wr_ms = (long)(ctx.write_us / 1000);
    total_paras += ctx.para_count;
    if (ch_ms > slowest_ms) {
      slowest_ms = ch_ms;
      slowest_ci = (int)ci;
    }
    // Print at most once per second
    if (esp_timer_get_time() - last_log_us >= 1000000LL) {
      ESP_LOGI("mrb", "ch %u/%u  %ldms  free=%lu", (unsigned)ci, (unsigned)book.chapter_count(), ch_ms,
               (unsigned long)esp_get_free_heap_size());
      last_log_us = esp_timer_get_time();
    }
    ctx.write_us = 0;
    ctx.para_count = 0;
#endif
  }

  // Remap TOC file_idx (zip entry index) → spine index so ReaderScreen::load_chapter_() works.
  // toc_work already has para_index values filled in from fragment anchor resolution.
  const auto& spine = book.epub().spine();
  for (auto& entry : toc_work.entries) {
    for (size_t si = 0; si < spine.size(); ++si) {
      if (spine[si].file_idx == entry.file_idx) {
        entry.file_idx = static_cast<uint16_t>(si);
        break;
      }
    }
  }

  // Build spine filename table: base filename of each spine item for href resolution at runtime.
  std::vector<std::string> spine_files;
  spine_files.reserve(spine.size());
  for (const auto& si : spine) {
    std::string_view entry_name = zip.entry(si.file_idx).name;
    auto slash_pos = entry_name.rfind('/');
    std::string basename =
        (slash_pos != std::string_view::npos) ? std::string(entry_name.substr(slash_pos + 1)) : std::string(entry_name);
    spine_files.push_back(std::move(basename));
  }

  bool ok = writer.finish(book.metadata(), toc_work, spine_files);
  writer.close();  // explicit close so fclose() happens before we return

#ifdef ESP_PLATFORM
  long total_ms = (long)((esp_timer_get_time() - total_start) / 1000);
  ESP_LOGI("mrb", "TOTAL conversion: %ldms (%u chapters, %u paras, slowest ch%d=%ldms) ok=%d", total_ms,
           (unsigned)book.chapter_count(), (unsigned)total_paras, slowest_ci, slowest_ms, ok);
#endif

  return ok;
}

#ifdef ESP_PLATFORM
void benchmark_epub_conversion(Book& book, const char* tmp_path, long open_ms, uint8_t* work_buf, uint8_t* xml_buf) {
  static constexpr const char* TAG = "bench";
  static constexpr size_t kWorkBufSize = ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 2048;
  static constexpr size_t kXmlBufSize = 16384;

  std::unique_ptr<uint8_t[]> owned_work, owned_xml;
  if (!work_buf) {
    owned_work = std::make_unique<uint8_t[]>(kWorkBufSize);
    work_buf = owned_work.get();
  }
  if (!xml_buf) {
    owned_xml = std::make_unique<uint8_t[]>(kXmlBufSize);
    xml_buf = owned_xml.get();
  }

  const ZipReader& zip = book.epub().zip();
  const auto& spine = book.epub().spine();
  IZipFile& file = book.file();
  const unsigned nch = (unsigned)book.chapter_count();

  ESP_LOGI(TAG, "=== EPUB BENCHMARK START ===");
  ESP_LOGI(TAG, "chapters=%u  free=%lu largest=%lu", nch, (unsigned long)esp_get_free_heap_size(),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  ESP_LOGI(TAG, "BENCH_OPEN: %ldms", open_ms);

  auto discard_cb = [](const uint8_t*, size_t, void*) -> bool { return true; };

  // BENCH_CONV: full streaming conversion (main metric)
  ESP_LOGI(TAG, "--- BENCH_CONV ---");
  int64_t t = esp_timer_get_time();
  bool ok = convert_epub_to_mrb_streaming(book, tmp_path, work_buf, xml_buf);
  long t_conv = (long)((esp_timer_get_time() - t) / 1000);
  long out_bytes = 0;
  if (ok) {
    FILE* f2 = fopen(tmp_path, "rb");
    if (f2) {
      fseek(f2, 0, SEEK_END);
      out_bytes = ftell(f2);
      fclose(f2);
      remove(tmp_path);
    }
  }
  ESP_LOGI(TAG, "BENCH_CONV: %ldms  output=%ldB  ok=%d", t_conv, out_bytes, ok);

  // BENCH_SEEK: raw SD seek cost — seeks to each chapter's local header, no inflate
  ESP_LOGI(TAG, "--- BENCH_SEEK (pure SD seek cost, %u seeks) ---", nch);
  uint8_t lhbuf[30];
  t = esp_timer_get_time();
  for (const auto& si : spine) {
    const ZipEntry& e = zip.entry(si.file_idx);
    file.seek(e.local_header_offset, SEEK_SET);
    file.read(lhbuf, sizeof(lhbuf));
  }
  long t_seek = (long)((esp_timer_get_time() - t) / 1000);
  ESP_LOGI(TAG, "BENCH_SEEK: %ldms  avg=%ldms/seek", t_seek, t_seek / (long)nch);

  // BENCH_DECOMP: decompress all chapters to /dev/null (seek + inflate, no parse)
  ESP_LOGI(TAG, "--- BENCH_DECOMP (decompress only, %u chapters) ---", nch);
  size_t decomp_bytes = 0;
  t = esp_timer_get_time();
  for (const auto& si : spine) {
    const ZipEntry& e = zip.entry(si.file_idx);
    decomp_bytes += e.uncompressed_size;
    zip.extract_streaming(file, e, discard_cb, nullptr, work_buf, kWorkBufSize);
  }
  long t_decomp = (long)((esp_timer_get_time() - t) / 1000);
  ESP_LOGI(TAG, "BENCH_DECOMP: %ldms  %uB", t_decomp, (unsigned)decomp_bytes);

  // BENCH_BUILD: decompress + XML parse + paragraph build (no write)
  ESP_LOGI(TAG, "--- BENCH_BUILD (decompress + XML + paragraph build) ---");
  unsigned total_paras = 0;
  auto count_sink = [](void* ctx, Paragraph&&) { ++(*(unsigned*)ctx); };
  t = esp_timer_get_time();
  for (size_t ci = 0; ci < book.chapter_count(); ++ci)
    book.load_chapter_streaming(ci, count_sink, &total_paras, work_buf, xml_buf);
  long t_build = (long)((esp_timer_get_time() - t) / 1000);
  ESP_LOGI(TAG, "BENCH_BUILD: %ldms  paras=%u", t_build, total_paras);

  // BENCH_WRITE: raw fwrite — isolates SD write throughput
  long t_write = 0;
  if (out_bytes > 0) {
    ESP_LOGI(TAG, "--- BENCH_WRITE (raw fwrite %ldB) ---", out_bytes);
    char wr_path[300];
    snprintf(wr_path, sizeof(wr_path), "%s.wr", tmp_path);
    FILE* wf = fopen(wr_path, "wb");
    if (wf) {
      static uint8_t dummy[4096];
      t = esp_timer_get_time();
      for (long rem = out_bytes; rem > 0;) {
        int want = rem < 4096 ? (int)rem : 4096;
        fwrite(dummy, 1, (size_t)want, wf);
        rem -= want;
      }
      fclose(wf);
      t_write = (long)((esp_timer_get_time() - t) / 1000);
      remove(wr_path);
      long kbps = t_write > 0 ? (long)(out_bytes / 1024) * 1000 / t_write : 0;
      ESP_LOGI(TAG, "BENCH_WRITE: %ldms  %ldKB/s", t_write, kbps);
    }
  }

  ESP_LOGI(TAG,
           "=== BENCHMARK DONE: OPEN=%ldms CONV=%ldms SEEK=%ldms DECOMP=%ldms BUILD=%ldms WRITE=%ldms ===", open_ms,
           t_conv, t_seek, t_decomp, t_build, t_write);
}

// ---------------------------------------------------------------------------
// benchmark_image_size_read
// ---------------------------------------------------------------------------

void benchmark_image_size_read(Book& book, uint8_t* work_buf) {
  static constexpr const char* TAG = "img_bench";

  IZipFile& file = book.file();
  const ZipReader& zip = book.epub().zip();

  ESP_LOGI(TAG, "=== IMAGE SIZE BENCHMARK START ===");
  ESP_LOGI(TAG, "zip_entries=%u  free=%lu largest=%lu", (unsigned)zip.entry_count(),
           (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  // Work buffer for tinfl decompressor + LZ dictionary (~44 KB).
  // Caller should pass queue.scratch_buf1() (48 KB) to avoid heap allocation.
  static constexpr size_t kWorkBufSize = ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 1024;
  std::unique_ptr<uint8_t[]> local_work;
  if (!work_buf) {
    local_work = std::make_unique<uint8_t[]>(kWorkBufSize);
    work_buf = local_work.get();
    if (!work_buf) {
      ESP_LOGE(TAG, "OOM: no work buffer");
      return;
    }
  }

  unsigned total = 0, ok = 0, fail = 0;
  int64_t t_total = 0;

  for (unsigned i = 0; i < zip.entry_count(); ++i) {
    const ZipEntry& entry = zip.entry(i);

    // Filter by file extension — skip XHTML/CSS/NCX without any I/O.
    auto name = entry.name;
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos)
      continue;
    auto ext = name.substr(dot + 1);
    if (ext != "jpg" && ext != "jpeg" && ext != "png" && ext != "JPG" && ext != "JPEG" && ext != "PNG")
      continue;

    ++total;

    // Stream-decompress only until we have the image dimensions, then stop.
    // ImageSizeStream state is ~32 bytes; no heap allocation for image data.
    // PNG needs 24 bytes; JPEG SOF is past all APP/EXIF/ICC segments (may be 20+ KB).
    ImageSizeStream stream;
    int64_t t0 = esp_timer_get_time();
    zip.extract_streaming(
        file, entry,
        [](const uint8_t* d, size_t n, void* ud) -> bool { return !static_cast<ImageSizeStream*>(ud)->feed(d, n); },
        &stream, work_buf, kWorkBufSize);
    int64_t us = esp_timer_get_time() - t0;
    t_total += us;

    const char* fmt_str = (ext == "png" || ext == "PNG") ? "PNG " : "JPEG";
    if (stream.ok()) {
      ++ok;
      ESP_LOGI(TAG, "  entry %3u %-4s  %5ux%-5u  %4ldus", i, fmt_str, stream.width(), stream.height(), (long)us);
    } else {
      ++fail;
      ESP_LOGI(TAG, "  entry %3u %-4s  ERR (no SOF found)", i, fmt_str);
    }
  }

  long avg_us = total > 0 ? (long)(t_total / total) : 0;
  ESP_LOGI(TAG, "=== IMAGE SIZE BENCH DONE: images=%u ok=%u fail=%u total=%ldms avg=%ldus ===", total, ok, fail,
           (long)(t_total / 1000), avg_us);
}

// ---------------------------------------------------------------------------
// benchmark_image_decode
// ---------------------------------------------------------------------------

void benchmark_image_decode(Book& book, uint8_t* work_buf) {
  static constexpr const char* TAG = "img_decode";
  // Full display resolution — same cap the ReaderScreen uses.
  // 480×800 → stride=60, output=48 000 bytes (fits in scratch_buf1).
  static constexpr uint16_t kMaxW = DrawBuffer::kWidth;
  static constexpr uint16_t kMaxH = DrawBuffer::kHeight;

  IZipFile& file = book.file();
  const ZipReader& zip = book.epub().zip();

  ESP_LOGI(TAG, "=== IMAGE DECODE TEST START ===");
  ESP_LOGI(TAG, "zip_entries=%u  free=%lu largest=%lu", (unsigned)zip.entry_count(),
           (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  // Work buffer for ZipEntryInput (~44 KB).  Caller passes queue.scratch_buf1().
  static constexpr size_t kWorkBufSize = ZipEntryInput::kDecompSize + ZipEntryInput::kDictSize + 1024;
  std::unique_ptr<uint8_t[]> local_work;
  if (!work_buf) {
    local_work = std::make_unique<uint8_t[]>(kWorkBufSize);
    work_buf = local_work.get();
    if (!work_buf) {
      ESP_LOGE(TAG, "OOM: no work buffer");
      return;
    }
  }

  unsigned total = 0, ok_count = 0, fail_count = 0;
  int64_t t_total = 0;

  for (unsigned i = 0; i < zip.entry_count(); ++i) {
    const ZipEntry& entry = zip.entry(i);
    auto name = entry.name;
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos)
      continue;
    auto ext = name.substr(dot + 1);
    bool is_jpeg = (ext == "jpg" || ext == "jpeg" || ext == "JPG" || ext == "JPEG");
    bool is_png = (ext == "png" || ext == "PNG");
    if (!is_jpeg && !is_png)
      continue;

    ++total;
    const char* fmt_str = is_jpeg ? "JPEG" : "PNG ";

    uint32_t free_before = esp_get_free_heap_size();
    int64_t t0 = esp_timer_get_time();

    DecodedImage decoded;
    ImageError err = book.decode_image(i, decoded, kMaxW, kMaxH, work_buf, kWorkBufSize);

    int64_t us = esp_timer_get_time() - t0;
    t_total += us;
    uint32_t free_after = esp_get_free_heap_size();

    if (err == ImageError::Ok) {
      ++ok_count;
      ESP_LOGI(TAG, "  entry %3u %-4s  %4ux%-4u  OK   %5ldms  heap_delta=%ld", i, fmt_str, decoded.width,
               decoded.height, (long)(us / 1000), (long)free_before - (long)free_after);
    } else {
      ++fail_count;
      ESP_LOGI(TAG, "  entry %3u %-4s          FAIL err=%d  %5ldms  heap_delta=%ld", i, fmt_str, (int)err,
               (long)(us / 1000), (long)free_before - (long)free_after);
    }
  }

  long avg_ms = total > 0 ? (long)(t_total / 1000 / total) : 0;
  ESP_LOGI(TAG, "=== IMAGE DECODE TEST DONE: images=%u ok=%u fail=%u total=%ldms avg=%ldms free=%lu===", total,
           ok_count, fail_count, (long)(t_total / 1000), avg_ms, (unsigned long)esp_get_free_heap_size());
}
#endif

}  // namespace microreader
