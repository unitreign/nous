#pragma once

#include <string>
#include <vector>

#include "ContentModel.h"
#include "CssParser.h"
#include "ZipReader.h"

namespace microreader {

// Callback type for streaming chapter parsing.
// Each paragraph is emitted as soon as it's parsed — no accumulation.
using ParagraphSink = void (*)(void* ctx, Paragraph&& para);

// Callback for element id="" annotations encountered during streaming XHTML parsing.
// Called once per element that has an id attribute.
// para_idx = number of paragraphs already emitted before this element opens.
using IdSink = void (*)(void* ctx, const char* id, size_t id_len, uint32_t para_idx);

enum class EpubError {
  Ok = 0,
  ContainerMissing,
  ContentOpfMissing,
  InvalidData,
  ZipError,
  XmlError,
};

// EPUB book: parsed from an EPUB file.
// Stores the ZIP entries, spine, metadata, and global stylesheet.
// Chapters are parsed on-demand via parse_chapter().
class Epub {
 public:
  Epub() = default;

  // Set CSS unit conversion config (call before open()).
  void set_css_config(const CssConfig& config) {
    stylesheet_.set_config(config);
  }
  const CssConfig& css_config() const {
    return stylesheet_.config();
  }

  // Open an EPUB file. work_buf (~45KB) and xml_buf (~4KB) are used during
  // OPF/NCX parsing. Caller must provide both; allocate them before calling.
  // If parse_css_ncx is false, skips extracting CSS and parsing NCX (fast for indexing).
  EpubError open(IZipFile& file, uint8_t* work_buf, uint8_t* xml_buf, bool parse_css_ncx = true);

  // Lightweight open: only parses the ZIP central directory.
  // No OPF/NCX/CSS parsing — only zip().entry() is available afterwards.
  // Sufficient for image decode operations.
  EpubError open_zip_only(IZipFile& file);

  // Release all parsed data (ZIP entries, spine, stylesheet, TOC, metadata).
  void close();

  // Number of chapters (spine items).
  size_t chapter_count() const {
    return spine_.size();
  }

  // Parse a specific chapter by index.
  EpubError parse_chapter(IZipFile& file, size_t index, Chapter& out) const;

  // Stream-parse a chapter: paragraphs are emitted one at a time via sink.
  // Uses ~37KB working memory instead of extracting the full XHTML.
  EpubError parse_chapter_streaming(IZipFile& file, size_t index, ParagraphSink sink, void* sink_ctx, uint8_t* work_buf,
                                    uint8_t* xml_buf, IdSink id_sink = nullptr, void* id_sink_ctx = nullptr) const;

  // Access metadata.
  const EpubMetadata& metadata() const {
    return metadata_;
  }

  // Access TOC.
  const TableOfContents& toc() const {
    return toc_;
  }
  // Move TOC out (used by MRB converter to avoid a copy on fragmented heap).
  TableOfContents take_toc() {
    return std::move(toc_);
  }

  // Access the zip reader (for image extraction etc)
  const ZipReader& zip() const {
    return zip_;
  }
  const std::vector<SpineItem>& spine() const {
    return spine_;
  }
  const CssStylesheet& stylesheet() const {
    return stylesheet_;
  }

  // Resolve a path relative to a content file's directory.
  // e.g. resolve_path("OEBPS/chapters/", "../images/test.jpg") → "OEBPS/images/test.jpg"
  static std::string resolve_path(const std::string& base_dir, const std::string& href);

  // Find an entry index by path.
  const ZipEntry* find_entry(const std::string& path) const {
    return zip_.find(path);
  }
  int find_entry_index(const std::string& path) const;

 private:
  ZipReader zip_;
  std::string root_dir_;  // e.g. "OEBPS/"
  EpubMetadata metadata_;
  std::vector<SpineItem> spine_;
  TableOfContents toc_;
  CssStylesheet stylesheet_;
  int cover_idx_ = -1;

  // Internal parsing steps
  EpubError parse_container(IZipFile& file, std::string& rootfile_path, uint8_t* work_buf, size_t work_buf_size,
                            uint8_t* xml_buf, size_t xml_buf_size);
  EpubError parse_opf(IZipFile& file, const std::string& opf_path, uint8_t* work_buf, uint8_t* xml_buf,
                      bool parse_css_ncx);
};

// Parse XHTML body into paragraphs (used by Epub::parse_chapter, also
// usable standalone for testing).
EpubError parse_xhtml_body(const uint8_t* data, size_t size, const CssStylesheet* inline_css,
                           const CssStylesheet* extern_css, const std::string& base_dir, const ZipReader& zip,
                           std::vector<Paragraph>& out);

}  // namespace microreader
