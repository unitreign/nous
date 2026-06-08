#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "microreader/content/mrb/MrbConverter.h"
#include "microreader/content/mrb/MrbReader.h"
#include "microreader/content/mrb/MrbWriter.h"

namespace fs = std::filesystem;
using namespace microreader;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

class MrbFormatTest : public ::testing::Test {
 protected:
  std::string tmp_path_;

  void SetUp() override {
    tmp_path_ = (fs::temp_directory_path() / "mrb_test.mrb").string();
  }

  void TearDown() override {
    std::remove(tmp_path_.c_str());
  }

  // Build a simple text paragraph with one run.
  static Paragraph make_text(const std::string& text, FontStyle style = FontStyle::Regular) {
    TextParagraph tp;
    tp.runs.push_back(microreader::Run{text, style});
    return Paragraph::make_text(std::move(tp));
  }

  // Compare two runs field by field.
  static void expect_run_eq(const microreader::Run& a, const microreader::Run& b, const std::string& ctx) {
    EXPECT_EQ(a.text, b.text) << ctx;
    EXPECT_EQ(a.style, b.style) << ctx;
    EXPECT_EQ(a.size_pct, b.size_pct) << ctx;
    EXPECT_EQ(a.vertical_align, b.vertical_align) << ctx;
    EXPECT_EQ(a.breaking, b.breaking) << ctx;
    EXPECT_EQ(a.margin_left, b.margin_left) << ctx;
    EXPECT_EQ(a.margin_right, b.margin_right) << ctx;
  }

  // Compare two paragraphs field by field.
  static void expect_paragraph_eq(const Paragraph& a, const Paragraph& b, const std::string& ctx) {
    EXPECT_EQ(a.type, b.type) << ctx;
    EXPECT_EQ(a.spacing_before, b.spacing_before) << ctx;

    if (a.type == ParagraphType::Text) {
      EXPECT_EQ(a.text.alignment, b.text.alignment) << ctx;
      EXPECT_EQ(a.text.indent, b.text.indent) << ctx;
      EXPECT_EQ(a.text.line_height_pct, b.text.line_height_pct) << ctx;
      EXPECT_EQ(a.text.inline_image.has_value(), b.text.inline_image.has_value()) << ctx;
      if (a.text.inline_image.has_value() && b.text.inline_image.has_value()) {
        EXPECT_EQ(a.text.inline_image->key, b.text.inline_image->key) << ctx;
        EXPECT_EQ(a.text.inline_image->attr_width, b.text.inline_image->attr_width) << ctx;
        EXPECT_EQ(a.text.inline_image->attr_height, b.text.inline_image->attr_height) << ctx;
      }
      ASSERT_EQ(a.text.runs.size(), b.text.runs.size()) << ctx;
      for (size_t i = 0; i < a.text.runs.size(); ++i) {
        expect_run_eq(a.text.runs[i], b.text.runs[i], ctx + " run[" + std::to_string(i) + "]");
      }
    } else if (a.type == ParagraphType::Image) {
      EXPECT_EQ(a.image.key, b.image.key) << ctx;
    }
  }

  // Load the nth paragraph of a chapter via the descriptor table.
  static bool load_chapter_para(MrbReader& reader, uint16_t chapter_idx, uint32_t para_idx, Paragraph& out) {
    MrbChapterSource src(reader, chapter_idx);
    if (para_idx >= src.paragraph_count())
      return false;
    out = src.paragraph(para_idx);
    return true;
  }
};

// ---------------------------------------------------------------------------
// Round-trip: write paragraphs â†’ read back, compare
// ---------------------------------------------------------------------------

TEST_F(MrbFormatTest, RoundTrip_TextParagraph) {
  TextParagraph tp;
  tp.alignment = Alignment::Center;
  tp.indent = -24;
  tp.line_height_pct = 120;
  tp.runs.push_back(microreader::Run{"Hello, ", FontStyle::Regular});
  tp.runs.push_back(microreader::Run{"world!", FontStyle::Bold});
  Paragraph para = Paragraph::make_text(std::move(tp));
  para.spacing_before = 16;

  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(para));
  writer.end_chapter();

  EpubMetadata meta;
  meta.title = "Test Book";
  meta.author = "Author";
  meta.language = "en";
  TableOfContents toc;
  ASSERT_TRUE(writer.finish(meta, toc));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  EXPECT_EQ(reader.paragraph_count(), 1u);
  EXPECT_EQ(reader.chapter_count(), 1);

  Paragraph out;
  ASSERT_TRUE(load_chapter_para(reader, 0, 0, out));
  expect_paragraph_eq(para, out, "text paragraph");
}

TEST_F(MrbFormatTest, RoundTrip_ImageParagraph) {
  Paragraph para = Paragraph::make_image(42, 320, 240);
  para.spacing_before = 8;

  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  // Register image in image ref table (converter would do this).
  writer.add_image_ref(42, 320, 240);
  ASSERT_TRUE(writer.write_paragraph(para));
  writer.end_chapter();

  EpubMetadata meta;
  meta.title = "Img Book";
  ASSERT_TRUE(writer.finish(meta, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));

  Paragraph out;
  ASSERT_TRUE(load_chapter_para(reader, 0, 0, out));
  EXPECT_EQ(out.type, ParagraphType::Image);
  // Image key in the written paragraph was 42 (raw key, not remapped here).
  // The reader looks up dimensions from the image ref table by key index.
  // Since key=42 is out of range for image table (only 1 entry at index 0),
  // dimensions come from the stored 4-byte body which just has key+spacing.
  // For a proper round-trip the converter remaps keys to image table indices.
  EXPECT_EQ(out.spacing_before, para.spacing_before);
}

TEST_F(MrbFormatTest, RoundTrip_HrAndPageBreak) {
  Paragraph hr = Paragraph::make_hr();
  hr.spacing_before = 20;
  Paragraph pb = Paragraph::make_page_break();

  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(hr));
  ASSERT_TRUE(writer.write_paragraph(pb));
  writer.end_chapter();

  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  EXPECT_EQ(reader.paragraph_count(), 2u);

  Paragraph out_hr, out_pb;
  ASSERT_TRUE(load_chapter_para(reader, 0, 0, out_hr));
  ASSERT_TRUE(load_chapter_para(reader, 0, 1, out_pb));

  EXPECT_EQ(out_hr.type, ParagraphType::Hr);
  EXPECT_EQ(out_hr.spacing_before, hr.spacing_before);
  EXPECT_EQ(out_pb.type, ParagraphType::PageBreak);
}

TEST_F(MrbFormatTest, RoundTrip_MultipleChapters) {
  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));

  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(make_text("Chapter 1 para 1")));
  ASSERT_TRUE(writer.write_paragraph(make_text("Chapter 1 para 2")));
  writer.end_chapter();

  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(make_text("Chapter 2 para 1")));
  writer.end_chapter();

  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(make_text("Chapter 3 para 1")));
  ASSERT_TRUE(writer.write_paragraph(make_text("Chapter 3 para 2")));
  ASSERT_TRUE(writer.write_paragraph(make_text("Chapter 3 para 3")));
  writer.end_chapter();

  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  EXPECT_EQ(reader.chapter_count(), 3);
  EXPECT_EQ(reader.paragraph_count(), 6u);

  EXPECT_NE(reader.chapter_para_table_offset(0), 0u);
  EXPECT_EQ(reader.chapter_paragraph_count(0), 2);
  EXPECT_NE(reader.chapter_para_table_offset(1), 0u);
  EXPECT_EQ(reader.chapter_paragraph_count(1), 1);
  EXPECT_NE(reader.chapter_para_table_offset(2), 0u);
  EXPECT_EQ(reader.chapter_paragraph_count(2), 3);

  Paragraph out;
  ASSERT_TRUE(load_chapter_para(reader, 1, 0, out));
  EXPECT_EQ(out.type, ParagraphType::Text);
  ASSERT_EQ(out.text.runs.size(), 1u);
  EXPECT_EQ(out.text.runs[0].text, "Chapter 2 para 1");
}

TEST_F(MrbFormatTest, RoundTrip_AllRunStyles) {
  TextParagraph tp;
  tp.alignment = Alignment::Justify;
  tp.indent = 30;

  microreader::Run r1{"Regular text ", FontStyle::Regular, 100};
  microreader::Run r2{"Bold text ", FontStyle::Bold, 100};
  r2.margin_left = 12;
  microreader::Run r3{"Italic small", FontStyle::Italic, 100};
  r3.margin_right = 8;
  r3.vertical_align = VerticalAlign::Super;
  microreader::Run r4{"BoldItalic break", FontStyle::BoldItalic, 100, true};
  r4.vertical_align = VerticalAlign::Sub;

  tp.runs = {r1, r2, r3, r4};
  Paragraph para = Paragraph::make_text(std::move(tp));

  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(para));
  writer.end_chapter();
  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));

  Paragraph out;
  ASSERT_TRUE(load_chapter_para(reader, 0, 0, out));
  expect_paragraph_eq(para, out, "all styles");
}

TEST_F(MrbFormatTest, RoundTrip_InlineImage) {
  TextParagraph tp;
  tp.inline_image = ImageRef{5, 100, 80};
  tp.runs.push_back(microreader::Run{"Text after image"});
  Paragraph para = Paragraph::make_text(std::move(tp));

  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(para));
  writer.end_chapter();
  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));

  Paragraph out;
  ASSERT_TRUE(load_chapter_para(reader, 0, 0, out));
  ASSERT_TRUE(out.text.inline_image.has_value());
  EXPECT_EQ(out.text.inline_image->key, 5);
  EXPECT_EQ(out.text.inline_image->attr_width, 100);
  EXPECT_EQ(out.text.inline_image->attr_height, 80);
}

TEST_F(MrbFormatTest, RoundTrip_Metadata) {
  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(make_text("hello")));
  writer.end_chapter();

  EpubMetadata meta;
  meta.title = "My Great Book";
  meta.author = "Jane Doe";
  meta.language = "de";
  TableOfContents toc;
  toc.add_entry("Chapter One", 0, 0);
  toc.add_entry("Chapter Two", 5, 0);
  ASSERT_TRUE(writer.finish(meta, toc));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  EXPECT_EQ(reader.metadata().title, "My Great Book");
  EXPECT_EQ(reader.metadata().author.value_or(""), "Jane Doe");
  EXPECT_EQ(reader.metadata().language.value_or(""), "de");

  ASSERT_EQ(reader.toc().entries.size(), 2u);
  EXPECT_EQ(reader.toc().label_of(reader.toc().entries[0]), "Chapter One");
  EXPECT_EQ(reader.toc().entries[0].file_idx, 0);
  EXPECT_EQ(reader.toc().label_of(reader.toc().entries[1]), "Chapter Two");
  EXPECT_EQ(reader.toc().entries[1].file_idx, 5);
}

TEST_F(MrbFormatTest, RoundTrip_EmptyChapter) {
  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));

  writer.begin_chapter();
  writer.end_chapter();  // empty chapter

  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(make_text("content")));
  writer.end_chapter();

  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  EXPECT_EQ(reader.chapter_count(), 2);
  EXPECT_EQ(reader.chapter_paragraph_count(0), 0);
  EXPECT_EQ(reader.chapter_paragraph_count(1), 1);
}

TEST_F(MrbFormatTest, RoundTrip_UnicodeText) {
  auto para = make_text("ÃœnÃ¯cÃ¶dÃ« HÃ«llÃ¶ WÃ¶rld â€” æ—¥æœ¬èªžãƒ†ã‚¹ãƒˆ");

  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(para));
  writer.end_chapter();
  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  Paragraph out;
  ASSERT_TRUE(load_chapter_para(reader, 0, 0, out));
  EXPECT_EQ(out.text.runs[0].text, "ÃœnÃ¯cÃ¶dÃ« HÃ«llÃ¶ WÃ¶rld â€” æ—¥æœ¬èªžãƒ†ã‚¹ãƒˆ");
}

TEST_F(MrbFormatTest, RoundTrip_ImageRefTable) {
  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));

  uint16_t idx0 = writer.add_image_ref(1000, 640, 480);
  uint16_t idx1 = writer.add_image_ref(2500, 100, 100);
  uint16_t idx2 = writer.add_image_ref(30000, 800, 600);
  EXPECT_EQ(idx0, 0);
  EXPECT_EQ(idx1, 1);
  EXPECT_EQ(idx2, 2);

  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(make_text("placeholder")));
  writer.end_chapter();
  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  EXPECT_EQ(reader.image_count(), 3);

  EXPECT_EQ(reader.image_ref(0).local_header_offset, 1000u);
  EXPECT_EQ(reader.image_ref(0).width, 640);
  EXPECT_EQ(reader.image_ref(0).height, 480);
  EXPECT_EQ(reader.image_ref(1).local_header_offset, 2500u);
  EXPECT_EQ(reader.image_ref(2).width, 800);
}

// ---------------------------------------------------------------------------
// EPUB â†’ MRB conversion test (using fixture EPUBs)
// ---------------------------------------------------------------------------

#ifdef TEST_FIXTURES_DIR
static std::string fixture_path(const char* name) {
  return std::string(TEST_FIXTURES_DIR) + "/" + name;
}

TEST_F(MrbFormatTest, ConvertBasicEpub) {
  Book book;
  auto err = book.open(fixture_path("basic.epub").c_str());
  if (err != EpubError::Ok)
    GTEST_SKIP() << "basic.epub not found";

  ASSERT_TRUE(convert_epub_to_mrb_streaming(book, tmp_path_.c_str()));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  EXPECT_GT(reader.paragraph_count(), 0u);
  EXPECT_GT(reader.chapter_count(), 0);

  // Verify we can load every paragraph via the descriptor table.
  for (uint16_t ci = 0; ci < reader.chapter_count(); ++ci) {
    MrbChapterSource src(reader, ci);
    EXPECT_EQ(src.paragraph_count(), reader.chapter_paragraph_count(ci));
    for (uint16_t pi = 0; pi < src.paragraph_count(); ++pi) {
      const Paragraph& p = src.paragraph(pi);
      EXPECT_NE(p.type, ParagraphType::PageBreak) << "ch " << ci << " para " << pi;
      (void)p;
    }
  }
}

TEST_F(MrbFormatTest, ConvertMultiChapterEpub) {
  Book book;
  auto err = book.open(fixture_path("multi_chapter.epub").c_str());
  if (err != EpubError::Ok)
    GTEST_SKIP() << "multi_chapter.epub not found";

  ASSERT_TRUE(convert_epub_to_mrb_streaming(book, tmp_path_.c_str()));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  EXPECT_EQ(reader.chapter_count(), static_cast<uint16_t>(book.chapter_count()));

  // Verify paragraph counts match per chapter.
  for (uint16_t ci = 0; ci < reader.chapter_count(); ++ci) {
    Chapter ch;
    book.load_chapter(ci, ch);

    uint16_t mrb_count = reader.chapter_paragraph_count(ci);
    EXPECT_EQ(mrb_count, static_cast<uint16_t>(ch.paragraphs.size()))
        << "Chapter " << ci << " paragraph count mismatch";
  }
}
#endif

// ---------------------------------------------------------------------------
// Verify that Book + MrbReader can be re-opened without leaking memory.
// Simulates the ReaderScreen flow: open book A â†’ convert â†’ read MRB â†’ close
// â†’ open book B â†’ convert â†’ read MRB.
// ---------------------------------------------------------------------------

#ifdef TEST_FIXTURES_DIR
TEST_F(MrbFormatTest, ReopenBookReleasesResources) {
  auto path_a = fixture_path("basic.epub");
  auto path_b = fixture_path("multi_chapter.epub");
  auto mrb_a = (fs::temp_directory_path() / "reopen_a.mrb").string();
  auto mrb_b = (fs::temp_directory_path() / "reopen_b.mrb").string();

  // ---- Round 1: open book A ----
  {
    Book book;
    auto err = book.open(path_a.c_str());
    if (err != EpubError::Ok)
      GTEST_SKIP() << "basic.epub not found";
    ASSERT_GT(book.chapter_count(), 0u);

    ASSERT_TRUE(convert_epub_to_mrb_streaming(book, mrb_a.c_str()));

    MrbReader reader;
    ASSERT_TRUE(reader.open(mrb_a.c_str()));
    EXPECT_GT(reader.paragraph_count(), 0u);
    uint16_t ch_count_a = reader.chapter_count();
    EXPECT_GT(ch_count_a, 0);
  }
  // Book + MrbReader destructors run here â€” all resources released.

  // ---- Round 2: open book B ----
  {
    Book book;
    auto err = book.open(path_b.c_str());
    if (err != EpubError::Ok)
      GTEST_SKIP() << "multi_chapter.epub not found";
    ASSERT_GT(book.chapter_count(), 0u);

    ASSERT_TRUE(convert_epub_to_mrb_streaming(book, mrb_b.c_str()));

    MrbReader reader;
    ASSERT_TRUE(reader.open(mrb_b.c_str()));
    EXPECT_GT(reader.paragraph_count(), 0u);
    EXPECT_GT(reader.chapter_count(), 0);

    // Verify we can actually read paragraphs from book B.
    MrbChapterSource src(reader, 0);
    ASSERT_GT(src.paragraph_count(), 0u);
    const Paragraph& p = src.paragraph(0);
    EXPECT_EQ(p.type, ParagraphType::Text);
  }

  std::remove(mrb_a.c_str());
  std::remove(mrb_b.c_str());
}

// Verify Book::open can be called twice on the same instance (re-open).
TEST_F(MrbFormatTest, BookReopenSameInstance) {
  auto path_a = fixture_path("basic.epub");
  auto path_b = fixture_path("multi_chapter.epub");

  Book book;
  auto err = book.open(path_a.c_str());
  if (err != EpubError::Ok)
    GTEST_SKIP() << "basic.epub not found";
  size_t count_a = book.chapter_count();
  ASSERT_GT(count_a, 0u);

  // Re-open with a different book on the same Book instance.
  err = book.open(path_b.c_str());
  if (err != EpubError::Ok)
    GTEST_SKIP() << "multi_chapter.epub not found";
  size_t count_b = book.chapter_count();
  ASSERT_GT(count_b, 0u);

  // Verify chapter count changed (these are different books).
  // The important thing is that it doesn't crash.
  EXPECT_NE(count_a, count_b);
}
#endif

// ---------------------------------------------------------------------------
// Corruption / robustness: MrbReader::open() must fail gracefully.
// ---------------------------------------------------------------------------

// Opening a nonexistent file should return false.
TEST_F(MrbFormatTest, OpenNonexistentFile) {
  MrbReader reader;
  EXPECT_FALSE(reader.open("/nonexistent/path/does_not_exist.mrb"));
}

// Opening an empty file (0 bytes) should return false.
TEST_F(MrbFormatTest, OpenEmptyFile) {
  // Create a zero-byte file at tmp_path_.
  { std::ofstream f(tmp_path_, std::ios::binary); }

  MrbReader reader;
  EXPECT_FALSE(reader.open(tmp_path_.c_str()));
}

// Opening a file with wrong magic bytes should return false.
TEST_F(MrbFormatTest, OpenWrongMagic) {
  {
    std::ofstream f(tmp_path_, std::ios::binary);
    // Write 32 bytes of garbage (wrong magic).
    const char junk[32] = "THIS IS NOT A VALID MRB FILE!!!";
    f.write(junk, 32);
  }

  MrbReader reader;
  EXPECT_FALSE(reader.open(tmp_path_.c_str()));
}

// ---------------------------------------------------------------------------
// ProgressAccuracy: char-based vs paragraph-based percentage
//
// Scenario: two chapters with very unequal paragraph sizes.
//   Chapter 0: 1 short heading paragraph (5 chars)
//   Chapter 1: 1 long body paragraph (995 chars)
//
// Paragraph-based progress at the boundary: 1/(1+1) = 50%
// Char-based progress at the boundary:      5/(5+995) = 0.5% â†’ rounds to 0%
//
// This test verifies:
//   - char_count is stored and retrieved correctly
//   - MrbChapterSource::char_before_para() returns accurate cumulative offsets
//   - total_char_count() sums all chapters
// ---------------------------------------------------------------------------

TEST_F(MrbFormatTest, ProgressAccuracy_CharVsParagraph) {
  // Build two chapters with very different content sizes.
  const std::string short_text(5, 'x');   // 5 chars
  const std::string long_text(995, 'y');  // 995 chars

  {
    MrbWriter writer;
    ASSERT_TRUE(writer.open(tmp_path_.c_str()));

    writer.begin_chapter();
    ASSERT_TRUE(writer.write_paragraph(make_text(short_text)));
    writer.end_chapter();

    writer.begin_chapter();
    ASSERT_TRUE(writer.write_paragraph(make_text(long_text)));
    writer.end_chapter();

    ASSERT_TRUE(writer.finish({}, {}));
  }

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));

  // Verify stored char counts per chapter.
  EXPECT_EQ(reader.chapter_char_count(0), 5u);
  EXPECT_EQ(reader.chapter_char_count(1), 995u);
  EXPECT_EQ(reader.total_char_count(), 1000u);

  // Paragraph-based progress at end of chapter 0:
  //   paragraphs_before = 1, total_paragraphs = 2  â†’ 50%
  {
    uint32_t cur = 1;  // 1 paragraph in chapter 0
    int pct_para = static_cast<int>(cur * 100u / reader.paragraph_count());
    EXPECT_EQ(pct_para, 50);  // wildly wrong: only 0.5% of content read
  }

  // Char-based progress at end of chapter 0:
  //   chars_before_ch1 = 5, total_chars = 1000  â†’ 0%
  {
    uint64_t total = reader.total_char_count();
    uint64_t chars_before_ch1 = reader.chapter_char_count(0);
    int pct_char = static_cast<int>(chars_before_ch1 * 100u / total);
    EXPECT_EQ(pct_char, 0);  // correctly reflects we've read almost nothing
  }

  // MrbChapterSource: verify cumulative char offsets.
  {
    MrbChapterSource src0(reader, 0);
    EXPECT_EQ(src0.paragraph_count(), 1u);
    EXPECT_EQ(src0.char_before_para(0), 0u);
    EXPECT_EQ(src0.total_chars(), 5u);
  }
  {
    MrbChapterSource src1(reader, 1);
    EXPECT_EQ(src1.paragraph_count(), 1u);
    EXPECT_EQ(src1.char_before_para(0), 0u);
    EXPECT_EQ(src1.total_chars(), 995u);
  }
}

// Additional scenario: multiple paragraphs within a chapter, verify offsets.
TEST_F(MrbFormatTest, ProgressAccuracy_IntraChapterOffsets) {
  {
    MrbWriter writer;
    ASSERT_TRUE(writer.open(tmp_path_.c_str()));
    writer.begin_chapter();
    ASSERT_TRUE(writer.write_paragraph(make_text("abc")));    // 3 chars, offset 0
    ASSERT_TRUE(writer.write_paragraph(make_text("de")));     // 2 chars, offset 3
    ASSERT_TRUE(writer.write_paragraph(make_text("fghij")));  // 5 chars, offset 5
    writer.end_chapter();
    ASSERT_TRUE(writer.finish({}, {}));
  }

  MrbReader reader2;
  ASSERT_TRUE(reader2.open(tmp_path_.c_str()));
  EXPECT_EQ(reader2.chapter_char_count(0), 10u);
  EXPECT_EQ(reader2.total_char_count(), 10u);

  MrbChapterSource src(reader2, 0);
  EXPECT_EQ(src.paragraph_count(), 3u);
  EXPECT_EQ(src.char_before_para(0), 0u);
  EXPECT_EQ(src.char_before_para(1), 3u);
  EXPECT_EQ(src.char_before_para(2), 5u);
  EXPECT_EQ(src.total_chars(), 10u);
}

// ---------------------------------------------------------------------------
// Spine file table round-trip
// ---------------------------------------------------------------------------

TEST_F(MrbFormatTest, SpineFileTable_RoundTrip_EmptySpine) {
  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(make_text("hello")));
  writer.end_chapter();
  // finish() with default (empty) spine_files
  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  EXPECT_TRUE(reader.spine_files().empty());
}

TEST_F(MrbFormatTest, SpineFileTable_RoundTrip) {
  std::vector<std::string> spine = {"cover.xhtml", "chapter01.xhtml", "chapter02.xhtml", "chapter03.xhtml"};

  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  for (int i = 0; i < 4; ++i) {
    writer.begin_chapter();
    ASSERT_TRUE(writer.write_paragraph(make_text("para " + std::to_string(i))));
    writer.end_chapter();
  }
  ASSERT_TRUE(writer.finish({}, {}, spine));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  ASSERT_EQ(reader.spine_files().size(), spine.size());
  for (size_t i = 0; i < spine.size(); ++i)
    EXPECT_EQ(reader.spine_files()[i], spine[i]) << "index " << i;
}

TEST_F(MrbFormatTest, SpineFileTable_RoundTrip_AliceStructure) {
  // Matches alice-illustrated.epub spine: wrap + 14 numbered chapters.
  std::vector<std::string> spine = {
      "wrap0000.xhtml",
      "502862557236502936_28885-h-0.htm.xhtml",
      "502862557236502936_28885-h-1.htm.xhtml",
      "502862557236502936_28885-h-2.htm.xhtml",
      "502862557236502936_28885-h-3.htm.xhtml",
  };

  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  for (size_t i = 0; i < spine.size(); ++i) {
    writer.begin_chapter();
    ASSERT_TRUE(writer.write_paragraph(make_text("para")));
    writer.end_chapter();
  }
  ASSERT_TRUE(writer.finish({}, {}, spine));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  ASSERT_EQ(reader.spine_files().size(), spine.size());
  for (size_t i = 0; i < spine.size(); ++i)
    EXPECT_EQ(reader.spine_files()[i], spine[i]);

  // Verify basename matching works as LinksScreen::populate() would do it.
  // href "OEBPS/502862557236502936_28885-h-1.htm.xhtml|Page_13" → chapter 2
  const auto& sf = reader.spine_files();
  std::string href = "OEBPS/502862557236502936_28885-h-1.htm.xhtml|Page_13";
  auto pipe = href.find('|');
  std::string path_part = href.substr(0, pipe);
  auto slash = path_part.rfind('/');
  std::string basename = (slash != std::string::npos) ? path_part.substr(slash + 1) : path_part;
  uint16_t found_idx = 0xFFFF;
  for (size_t i = 0; i < sf.size(); ++i) {
    if (sf[i] == basename) {
      found_idx = static_cast<uint16_t>(i);
      break;
    }
  }
  EXPECT_EQ(found_idx, 2u) << "Link to *-h-1.htm.xhtml should resolve to spine[2], not " << found_idx;
}

// ---------------------------------------------------------------------------
// Anchor table round-trip
// ---------------------------------------------------------------------------

TEST_F(MrbFormatTest, AnchorTable_RoundTrip_NoAnchors) {
  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(make_text("text")));
  writer.end_chapter();
  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  uint16_t para = 99;
  EXPECT_FALSE(reader.find_anchor(0, "anything", 8, para));
  EXPECT_EQ(para, 99u) << "para should be unchanged when find_anchor returns false";
}

TEST_F(MrbFormatTest, AnchorTable_RoundTrip_SingleAnchor) {
  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  for (int i = 0; i < 8; ++i)
    ASSERT_TRUE(writer.write_paragraph(make_text("para " + std::to_string(i))));
  writer.end_chapter();
  writer.add_anchor(0, 5, "section1", 8);
  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  uint16_t para = 0;
  ASSERT_TRUE(reader.find_anchor(0, "section1", 8, para));
  EXPECT_EQ(para, 5u);
}

TEST_F(MrbFormatTest, AnchorTable_RoundTrip_MultiChapter) {
  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  for (int ch = 0; ch < 3; ++ch) {
    writer.begin_chapter();
    for (int p = 0; p < 10; ++p)
      ASSERT_TRUE(writer.write_paragraph(make_text("p")));
    writer.end_chapter();
  }
  // Add anchors across chapters
  writer.add_anchor(0, 2, "intro", 5);
  writer.add_anchor(1, 7, "Page_13", 7);
  writer.add_anchor(2, 0, "start", 5);
  writer.add_anchor(2, 9, "end", 3);
  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));

  uint16_t para = 0;
  ASSERT_TRUE(reader.find_anchor(0, "intro", 5, para));
  EXPECT_EQ(para, 2u);
  ASSERT_TRUE(reader.find_anchor(1, "Page_13", 7, para));
  EXPECT_EQ(para, 7u);
  ASSERT_TRUE(reader.find_anchor(2, "start", 5, para));
  EXPECT_EQ(para, 0u);
  ASSERT_TRUE(reader.find_anchor(2, "end", 3, para));
  EXPECT_EQ(para, 9u);

  // Non-existent anchor: wrong chapter
  EXPECT_FALSE(reader.find_anchor(0, "Page_13", 7, para));
  // Non-existent anchor: wrong id
  EXPECT_FALSE(reader.find_anchor(1, "missing", 7, para));
}

TEST_F(MrbFormatTest, AnchorTable_RoundTrip_LongId) {
  // Edge case: id at maximum length (255 bytes)
  std::string long_id(255, 'x');
  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  writer.begin_chapter();
  ASSERT_TRUE(writer.write_paragraph(make_text("p")));
  writer.end_chapter();
  writer.add_anchor(0, 0, long_id.c_str(), long_id.size());
  ASSERT_TRUE(writer.finish({}, {}));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  uint16_t para = 0;
  ASSERT_TRUE(reader.find_anchor(0, long_id.c_str(), long_id.size(), para));
  EXPECT_EQ(para, 0u);
}

TEST_F(MrbFormatTest, AnchorTable_SpineFiles_Together) {
  // Both spine file table and anchor table present in the same MRB.
  std::vector<std::string> spine = {"cover.xhtml", "part1.xhtml", "part2.xhtml"};

  MrbWriter writer;
  ASSERT_TRUE(writer.open(tmp_path_.c_str()));
  for (size_t i = 0; i < spine.size(); ++i) {
    writer.begin_chapter();
    ASSERT_TRUE(writer.write_paragraph(make_text("chapter " + std::to_string(i))));
    writer.end_chapter();
  }
  writer.add_anchor(1, 0, "ch1start", 8);
  writer.add_anchor(2, 3, "section2", 8);
  ASSERT_TRUE(writer.finish({}, {}, spine));

  MrbReader reader;
  ASSERT_TRUE(reader.open(tmp_path_.c_str()));
  ASSERT_EQ(reader.spine_files().size(), spine.size());
  EXPECT_EQ(reader.spine_files()[0], "cover.xhtml");
  EXPECT_EQ(reader.spine_files()[1], "part1.xhtml");
  EXPECT_EQ(reader.spine_files()[2], "part2.xhtml");

  uint16_t para = 0;
  ASSERT_TRUE(reader.find_anchor(1, "ch1start", 8, para));
  EXPECT_EQ(para, 0u);
  ASSERT_TRUE(reader.find_anchor(2, "section2", 8, para));
  EXPECT_EQ(para, 3u);
  EXPECT_FALSE(reader.find_anchor(0, "ch1start", 8, para));
}
