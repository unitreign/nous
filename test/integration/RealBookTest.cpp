// Integration tests using REAL EPUB files.
// These tests verify the full pipeline: ZIP → XML → CSS → EPUB → Chapters → Images
//
// Test books are sourced from the microreader/resources/books and TrustyReader/sd dirs.
// Tests are written defensively: if a book file doesn't exist, test is skipped.

#include <gtest/gtest.h>

// Macro: open a book or skip the test if not found
#define OPEN_BOOK_OR_SKIP(path)                      \
  do {                                               \
    if (!try_open(path)) {                           \
      if (skip_)                                     \
        GTEST_SKIP() << "Book not found: " << path_; \
      return;                                        \
    }                                                \
  } while (0)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "TestBooks.h"
#include "microreader/content/Book.h"
#include "microreader/content/ContentModel.h"
#include "microreader/content/EpubParser.h"
#include "microreader/content/ImageDecoder.h"
#include "microreader/content/ZipReader.h"

using namespace microreader;

// ---------------------------------------------------------------------------
// Paths to real EPUB files
// ---------------------------------------------------------------------------

// Resolve workspace root from TEST_FIXTURES_DIR (which is microreader2/test/fixtures)
static std::string workspace_root() {
  std::string fixtures = TEST_FIXTURES_DIR;
  // fixtures = .../microreader2/test/fixtures
  // workspace = .../microreader  (3 levels up)
  auto pos = fixtures.rfind('/');
  if (pos == std::string::npos)
    pos = fixtures.rfind('\\');
  std::string up1 = fixtures.substr(0, pos);  // .../microreader2/test
  pos = up1.rfind('/');
  if (pos == std::string::npos)
    pos = up1.rfind('\\');
  std::string up2 = up1.substr(0, pos);  // .../microreader2
  pos = up2.rfind('/');
  if (pos == std::string::npos)
    pos = up2.rfind('\\');
  return up2.substr(0, pos);  // .../microreader (workspace root)
}

static bool file_exists(const std::string& path) {
  std::ifstream f(path);
  return f.good();
}

// Helper: check that a chapter has at least some content
static void expect_chapter_has_content(const Chapter& ch, size_t chapter_idx) {
  // A valid chapter should have at least one paragraph
  EXPECT_GT(ch.paragraphs.size(), 0u) << "Chapter " << chapter_idx << " has no paragraphs";
}

// Helper: count paragraph types
struct ParagraphStats {
  size_t text = 0;
  size_t images = 0;
  size_t hrs = 0;
  size_t total_runs = 0;
  size_t total_chars = 0;
  bool has_bold = false;
  bool has_italic = false;
};

static ParagraphStats count_paragraphs(const Chapter& ch) {
  ParagraphStats stats;
  for (auto& p : ch.paragraphs) {
    switch (p.type) {
      case ParagraphType::Text:
        stats.text++;
        for (auto& r : p.text.runs) {
          stats.total_runs++;
          stats.total_chars += r.text.size();
          if (r.style == FontStyle::Bold || r.style == FontStyle::BoldItalic)
            stats.has_bold = true;
          if (r.style == FontStyle::Italic || r.style == FontStyle::BoldItalic)
            stats.has_italic = true;
        }
        break;
      case ParagraphType::Image:
        stats.images++;
        break;
      case ParagraphType::Hr:
        stats.hrs++;
        break;
    }
  }
  return stats;
}

// ===========================================================================
// Test fixture for real books
// ===========================================================================

class RealBookTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = workspace_root();
  }

  // Try to open a book. Returns true if successful, false if file doesn't exist.
  // Caller should check and return early if false.
  bool try_open(const std::string& relative_path) {
    path_ = root_ + "/" + relative_path;
    if (!file_exists(path_)) {
      skip_ = true;
      return false;
    }
    auto err = book_.open(path_.c_str());
    EXPECT_EQ(err, EpubError::Ok) << "Failed to open: " << path_;
    return err == EpubError::Ok;
  }

  bool skip_ = false;

  // Parse all chapters and collect stats
  void verify_all_chapters() {
    ASSERT_GT(book_.chapter_count(), 0u) << "No chapters in " << path_;

    size_t total_paragraphs = 0;
    size_t total_images = 0;
    size_t total_chars = 0;
    size_t empty_chapters = 0;

    for (size_t i = 0; i < book_.chapter_count(); ++i) {
      Chapter ch;
      auto err = book_.load_chapter(i, ch);
      EXPECT_EQ(err, EpubError::Ok) << "Failed chapter " << i << " in " << path_;
      if (err != EpubError::Ok)
        continue;

      auto stats = count_paragraphs(ch);
      total_paragraphs += stats.text;
      total_images += stats.images;
      total_chars += stats.total_chars;

      if (ch.paragraphs.empty())
        empty_chapters++;
    }

    // At least some bulk content
    EXPECT_GT(total_paragraphs, 0u) << "No text paragraphs in " << path_;
    EXPECT_GT(total_chars, 100u) << "Very little text in " << path_;

    printf("  [%s]\n", path_.c_str());
    printf("    Chapters: %zu, Paragraphs: %zu, Images: %zu, Chars: %zu, Empty: %zu\n", book_.chapter_count(),
           total_paragraphs, total_images, total_chars, empty_chapters);
  }

  Book book_;
  std::string root_;
  std::string path_;
};

// ===========================================================================
// Bobiverse — English fiction, multi-chapter, no images expected
// ===========================================================================

TEST_F(RealBookTest, Bobiverse_Open) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/bobiverse one.epub");

  EXPECT_FALSE(book_.metadata().title.empty());
  EXPECT_GT(book_.chapter_count(), 1u);
  printf("  Title: %s\n", book_.metadata().title.c_str());
  if (book_.metadata().author)
    printf("  Author: %s\n", book_.metadata().author->c_str());
  printf("  Chapters: %zu\n", book_.chapter_count());
}

TEST_F(RealBookTest, Bobiverse_AllChapters) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/bobiverse one.epub");
  verify_all_chapters();
}

TEST_F(RealBookTest, Bobiverse_FirstChapterHasText) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/bobiverse one.epub");

  // First chapter with actual text (skip cover/title pages)
  for (size_t i = 0; i < book_.chapter_count() && i < 5; ++i) {
    Chapter ch;
    ASSERT_EQ(book_.load_chapter(i, ch), EpubError::Ok);
    auto stats = count_paragraphs(ch);
    if (stats.total_chars > 10) {
      printf("  First text chapter: %zu (%zu chars)\n", i, stats.total_chars);
      return;
    }
  }
  // At least one of the first 5 chapters should have text
  FAIL() << "No chapter with text found in first 5 chapters";
}

#include "microreader/content/mrb/MrbConverter.h"

TEST_F(RealBookTest, SnowCrash) {
  OPEN_BOOK_OR_SKIP("microreader2/sd/books/snow crash ger.epub");
  verify_all_chapters();
  bool ok = convert_epub_to_mrb_streaming(book_, "snow_crash_ger_test.mrb");
  EXPECT_TRUE(ok);
}

TEST_F(RealBookTest, Bobiverse_TOC) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/bobiverse one.epub");

  auto& toc = book_.toc();
  // Typically has TOC entries
  printf("  TOC entries: %zu\n", toc.entries.size());
  for (size_t i = 0; i < std::min(toc.entries.size(), size_t(5)); ++i) {
    printf("    [%zu] %s\n", i, toc.entries[i].label.c_str());
  }
}

// ===========================================================================
// Snow Crash — English fiction, may have cover image
// ===========================================================================

TEST_F(RealBookTest, SnowCrash_Open) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/Snow Crash - Neal Stephenson.epub");

  EXPECT_FALSE(book_.metadata().title.empty());
  printf("  Title: %s\n", book_.metadata().title.c_str());
  printf("  Chapters: %zu\n", book_.chapter_count());
}

TEST_F(RealBookTest, SnowCrash_AllChapters) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/Snow Crash - Neal Stephenson.epub");
  verify_all_chapters();
}

TEST_F(RealBookTest, SnowCrash_ImageDecoding) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/Snow Crash - Neal Stephenson.epub");

  // Scan all chapters for images and try to decode them
  size_t decoded = 0;
  for (size_t i = 0; i < book_.chapter_count(); ++i) {
    Chapter ch;
    if (book_.load_chapter(i, ch) != EpubError::Ok)
      continue;
    for (auto& p : ch.paragraphs) {
      if (p.type == ParagraphType::Image) {
        DecodedImage img;
        auto err = book_.decode_image(p.image.key, img);
        if (err == ImageError::Ok) {
          EXPECT_GT(img.width, 0u);
          EXPECT_GT(img.height, 0u);
          EXPECT_FALSE(img.data.empty());
          decoded++;
          printf("    Decoded image entry %u: %ux%u\n", p.image.key, img.width, img.height);
        }
      }
    }
  }
  printf("  Total images decoded: %zu\n", decoded);
}

// ===========================================================================
// Bible (Luther 1912) — German, very large, many chapters
// ===========================================================================

TEST_F(RealBookTest, Bible_Open) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/bible_luther1912.epub");

  printf("  Title: %s\n", book_.metadata().title.c_str());
  printf("  Chapters: %zu\n", book_.chapter_count());
  EXPECT_GT(book_.chapter_count(), 10u);  // Bible has many chapters
}

TEST_F(RealBookTest, Bible_FirstAndLastChapter) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/bible_luther1912.epub");

  // First chapter with text (skip cover pages)
  {
    bool found_text = false;
    for (size_t i = 0; i < book_.chapter_count() && i < 5; ++i) {
      Chapter ch;
      ASSERT_EQ(book_.load_chapter(i, ch), EpubError::Ok);
      auto stats = count_paragraphs(ch);
      printf("  Chapter %zu: %zu paragraphs, %zu chars\n", i, stats.text, stats.total_chars);
      if (stats.total_chars > 0) {
        found_text = true;
        break;
      }
    }
    EXPECT_TRUE(found_text) << "No chapter with text in first 5 chapters";
  }

  // Last chapter
  {
    Chapter ch;
    size_t last = book_.chapter_count() - 1;
    ASSERT_EQ(book_.load_chapter(last, ch), EpubError::Ok);
    auto stats = count_paragraphs(ch);
    printf("  Last chapter (%zu): %zu paragraphs, %zu chars\n", last, stats.text, stats.total_chars);
  }
}

TEST_F(RealBookTest, Bible_AllChapters) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/bible_luther1912.epub");
  verify_all_chapters();
}

// ===========================================================================
// Mabuse — German fiction (from TrustyReader SD)
// ===========================================================================

TEST_F(RealBookTest, Mabuse_Open) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/mabuse.epub");

  printf("  Title: %s\n", book_.metadata().title.c_str());
  printf("  Chapters: %zu\n", book_.chapter_count());
}

TEST_F(RealBookTest, Mabuse_AllChapters) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/mabuse.epub");
  verify_all_chapters();
}

// ===========================================================================
// Ohler — German non-fiction
// ===========================================================================

TEST_F(RealBookTest, Ohler_Open) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/ohler.epub");

  printf("  Title: %s\n", book_.metadata().title.c_str());
  printf("  Chapters: %zu\n", book_.chapter_count());
}

TEST_F(RealBookTest, Ohler_AllChapters) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/ohler.epub");
  verify_all_chapters();
}

// ===========================================================================
// Eyes of the Void — English sci-fi, likely has cover
// ===========================================================================

TEST_F(RealBookTest, EyesOfTheVoid_Open) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/Eyes of the Void.epub");

  printf("  Title: %s\n", book_.metadata().title.c_str());
  if (book_.metadata().author)
    printf("  Author: %s\n", book_.metadata().author->c_str());
  printf("  Chapters: %zu\n", book_.chapter_count());
}

TEST_F(RealBookTest, EyesOfTheVoid_AllChapters) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/Eyes of the Void.epub");
  verify_all_chapters();
}

TEST_F(RealBookTest, EyesOfTheVoid_ImageDecoding) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/Eyes of the Void.epub");

  size_t decoded = 0;
  size_t failed = 0;
  for (size_t i = 0; i < book_.chapter_count(); ++i) {
    Chapter ch;
    if (book_.load_chapter(i, ch) != EpubError::Ok)
      continue;
    for (auto& p : ch.paragraphs) {
      if (p.type == ParagraphType::Image) {
        DecodedImage img;
        auto err = book_.decode_image(p.image.key, img);
        if (err == ImageError::Ok) {
          decoded++;
        } else {
          failed++;
          printf("    Image decode FAILED for entry %u: error %d\n", p.image.key, (int)err);
        }
      }
    }
  }
  printf("  Images decoded: %zu, failed: %zu\n", decoded, failed);
}

// ===========================================================================
// Dictator's Handbook — English non-fiction
// ===========================================================================

TEST_F(RealBookTest, DictatorsHandbook_Open) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/The Dictators Handbook.epub");

  printf("  Title: %s\n", book_.metadata().title.c_str());
  printf("  Chapters: %zu\n", book_.chapter_count());
}

TEST_F(RealBookTest, DictatorsHandbook_AllChapters) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/The Dictators Handbook.epub");
  verify_all_chapters();
}

// ===========================================================================
// Latin-style footnotes test — EPUB with footnotes
// ===========================================================================

TEST_F(RealBookTest, LatinFootnotes_Open) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/latin-style-footnotes-test.epub");

  printf("  Title: %s\n", book_.metadata().title.c_str());
  printf("  Chapters: %zu\n", book_.chapter_count());
}

TEST_F(RealBookTest, LatinFootnotes_AllChapters) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/latin-style-footnotes-test.epub");
  verify_all_chapters();
}

// ===========================================================================
// Bobiverse German (Z-Library) — German translation, potentially different encoding
// ===========================================================================

TEST_F(RealBookTest, BobiverseGerman_Open) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/Bobiverse 1 Ich bin viele (Dennis E. Taylor) (Z-Library).epub");

  printf("  Title: %s\n", book_.metadata().title.c_str());
  if (book_.metadata().language)
    printf("  Language: %s\n", book_.metadata().language->c_str());
  printf("  Chapters: %zu\n", book_.chapter_count());
}

TEST_F(RealBookTest, BobiverseGerman_AllChapters) {
  OPEN_BOOK_OR_SKIP("microreader/resources/books/Bobiverse 1 Ich bin viele (Dennis E. Taylor) (Z-Library).epub");
  verify_all_chapters();
}

// ===========================================================================
// Snow Crash (TrustyReader version)
// ===========================================================================

TEST_F(RealBookTest, SnowCrashTrusty_Open) {
  OPEN_BOOK_OR_SKIP("TrustyReader/sd/snow crash.epub");

  printf("  Title: %s\n", book_.metadata().title.c_str());
  printf("  Chapters: %zu\n", book_.chapter_count());
}

TEST_F(RealBookTest, SnowCrashTrusty_AllChapters) {
  OPEN_BOOK_OR_SKIP("TrustyReader/sd/snow crash.epub");
  verify_all_chapters();
}

// ===========================================================================
// Stress test: verify no crashes on all chapters of all available books
// ===========================================================================

TEST_F(RealBookTest, StressTest_AllBooks) {
  std::vector<std::string> book_paths = {
      "microreader/resources/books/bobiverse one.epub",
      "microreader/resources/books/Snow Crash - Neal Stephenson.epub",
      "microreader/resources/books/bible_luther1912.epub",
      "microreader/resources/books/mabuse.epub",
      "microreader/resources/books/ohler.epub",
      "microreader/resources/books/Eyes of the Void.epub",
      "microreader/resources/books/The Dictators Handbook.epub",
      "microreader/resources/books/snow crash.epub",
      "microreader/resources/books/snow crash eng.epub",
      "microreader/resources/books/snow crash ger.epub",
      "microreader/resources/books/latin-style-footnotes-test.epub",
      "microreader/resources/books/Bobiverse 1 Ich bin viele (Dennis E. Taylor) (Z-Library).epub",
      "TrustyReader/sd/snow crash.epub",
      "TrustyReader/sd/mabuse.epub",
      "TrustyReader/sd/ohler.epub",
  };

  size_t books_tested = 0;
  size_t books_skipped = 0;
  size_t total_chapters = 0;
  size_t total_paragraphs = 0;
  size_t total_images_found = 0;
  size_t total_images_decoded = 0;

  for (auto& rel : book_paths) {
    std::string full = root_ + "/" + rel;
    if (!file_exists(full)) {
      books_skipped++;
      continue;
    }

    Book b;
    auto err = b.open(full.c_str());
    if (err != EpubError::Ok) {
      ADD_FAILURE() << "Failed to open: " << rel << " error=" << (int)err;
      continue;
    }

    books_tested++;
    total_chapters += b.chapter_count();

    for (size_t i = 0; i < b.chapter_count(); ++i) {
      Chapter ch;
      err = b.load_chapter(i, ch);
      if (err != EpubError::Ok) {
        ADD_FAILURE() << "Chapter " << i << " failed in " << rel << " error=" << (int)err;
        continue;
      }

      for (auto& p : ch.paragraphs) {
        if (p.type == ParagraphType::Text)
          total_paragraphs++;
        if (p.type == ParagraphType::Image) {
          total_images_found++;
          DecodedImage img;
          if (b.decode_image(p.image.key, img) == ImageError::Ok) {
            total_images_decoded++;
          }
        }
      }
    }
  }

  printf("\n  === STRESS TEST SUMMARY ===\n");
  printf("  Books tested: %zu, skipped: %zu\n", books_tested, books_skipped);
  printf("  Total chapters: %zu\n", total_chapters);
  printf("  Total text paragraphs: %zu\n", total_paragraphs);
  printf("  Total images found: %zu, decoded: %zu\n", total_images_found, total_images_decoded);
  printf("  ===========================\n");

  EXPECT_GT(books_tested, 0u) << "No real books found for testing";
}

// ===========================================================================
// Image format + size validation � alice-illustrated (many illustrations)
// ===========================================================================

TEST_F(RealBookTest, AliceIllustrated_ImageSizes) {
  OPEN_BOOK_OR_SKIP("microreader2/test/books/gutenberg/alice-illustrated.epub");

  size_t images_found = 0;
  size_t images_resolved = 0;
  size_t images_unresolved = 0;
  size_t mismatches = 0;

  printf("  %-8s  %-6s  %s\n", "entry", "format", "dimensions");

  for (size_t i = 0; i < book_.chapter_count(); ++i) {
    Chapter ch;
    if (book_.load_chapter(i, ch) != EpubError::Ok)
      continue;

    for (auto& p : ch.paragraphs) {
      auto check_image = [&](const ImageRef& img) {
        ++images_found;

        // Read format + size directly from raw bytes.
        std::vector<uint8_t> raw;
        if (book_.extract_entry(img.key, raw) != ZipError::Ok || raw.empty())
          return;

        auto fmt = guess_format_from_magic(raw.data(), raw.size());
        const char* fmt_str = (fmt == ImageFormat::Jpeg) ? "JPEG" : (fmt == ImageFormat::Png) ? "PNG" : "unknown";
        uint16_t w = 0, h = 0;
        auto sz_err = read_image_size(raw.data(), raw.size(), w, h);
        if (sz_err != ImageError::Ok) {
          printf("  entry %3u: %-6s  SIZE READ FAILED (error %d)\n", img.key, fmt_str, (int)sz_err);
          ++images_unresolved;
          return;
        }

        printf("  entry %3u: %-6s  %ux%u\n", img.key, fmt_str, w, h);
        ++images_resolved;

        // Cross-check: parse_chapter should have resolved the same dimensions.
        if (img.attr_width > 0 && img.attr_height > 0) {
          EXPECT_EQ(w, img.attr_width) << "Width mismatch for entry " << img.key;
          EXPECT_EQ(h, img.attr_height) << "Height mismatch for entry " << img.key;
          if (w != img.attr_width || h != img.attr_height)
            ++mismatches;
        }
      };

      if (p.type == ParagraphType::Image)
        check_image(p.image);
      if (p.type == ParagraphType::Text && p.text.inline_image.has_value())
        check_image(*p.text.inline_image);
    }
  }

  printf("\n  Images found: %zu  resolved: %zu  unresolved: %zu  mismatches: %zu\n", images_found, images_resolved,
         images_unresolved, mismatches);

  EXPECT_GT(images_found, 0u) << "alice-illustrated should have images";
  EXPECT_EQ(images_unresolved, 0u) << "All images should have readable dimensions";
  EXPECT_EQ(mismatches, 0u) << "Direct read_image_size must match chapter-pipeline result";
}

// ===========================================================================
// Bulk image size resolution � all curated test books
// ===========================================================================

TEST_F(RealBookTest, BulkImageSizeResolution) {
  auto books = test_books::get_curated_books();
  if (books.empty())
    GTEST_SKIP() << "No curated test books found";

  printf("\n  %-44s  %6s  %6s  %6s\n", "Book", "images", "ok", "zero");
  printf("  %s\n", std::string(66, '-').c_str());

  size_t total_found = 0, total_ok = 0, total_zero = 0;

  for (const auto& epub_path : books) {
    auto stem = std::filesystem::path(epub_path).stem().string();
    Book b;
    if (b.open(epub_path.c_str()) != EpubError::Ok)
      continue;

    size_t found = 0, ok = 0, zero = 0;
    for (size_t i = 0; i < b.chapter_count(); ++i) {
      Chapter ch;
      if (b.load_chapter(i, ch) != EpubError::Ok)
        continue;
      for (auto& p : ch.paragraphs) {
        auto tally = [&](const ImageRef& img) {
          ++found;
          if (img.attr_width > 0 && img.attr_height > 0)
            ++ok;
          else
            ++zero;
        };
        if (p.type == ParagraphType::Image)
          tally(p.image);
        if (p.type == ParagraphType::Text && p.text.inline_image.has_value())
          tally(*p.text.inline_image);
      }
    }

    if (found > 0)
      printf("  %-44s  %6zu  %6zu  %6zu\n", stem.c_str(), found, ok, zero);
    total_found += found;
    total_ok += ok;
    total_zero += zero;
  }

  printf("  %s\n", std::string(66, '-').c_str());
  printf("  %-44s  %6zu  %6zu  %6zu\n", "TOTAL", total_found, total_ok, total_zero);
}

// ===========================================================================
// Tress of the Emerald Sea — diagnostic TOC check
// ===========================================================================

TEST_F(RealBookTest, Tress1_TOC) {
  OPEN_BOOK_OR_SKIP("microreader/sd/books/Tress of the Emerald Sea 1.epub");

  auto& toc = book_.toc();
  printf("  Title: %s\n", book_.metadata().title.c_str());
  printf("  Chapters (spine): %zu\n", book_.chapter_count());
  printf("  TOC entries: %zu\n", toc.entries.size());
  for (size_t i = 0; i < toc.entries.size(); ++i) {
    const auto& e = toc.entries[i];
    printf("    [%zu] depth=%u file_idx=%u label='%s'\n", i, (unsigned)e.depth, (unsigned)e.file_idx, e.label.c_str());
  }
}

// ===========================================================================
// MRB image decode via read_local_entry + sink — mirrors ReaderScreen pipeline
// ===========================================================================

#include "microreader/content/mrb/MrbConverter.h"
#include "microreader/content/mrb/MrbReader.h"

TEST_F(RealBookTest, AliceIllustrated_MrbImageDecode_SinkPath) {
  // This test exercises the exact path ReaderScreen uses to decode images:
  // MRB open ? get image_ref ? read_local_entry from EPUB ? decode with sink.
  OPEN_BOOK_OR_SKIP("microreader2/test/books/gutenberg/alice-illustrated.epub");

  // Convert to MRB if needed.
  std::string mrb_path = path_;
  auto dot = mrb_path.rfind('.');
  if (dot != std::string::npos)
    mrb_path = mrb_path.substr(0, dot);
  mrb_path += ".test_sink.mrb";

  // Always generate fresh MRB for the test.
  ASSERT_TRUE(convert_epub_to_mrb_streaming(book_, mrb_path.c_str())) << "MRB conversion failed";

  MrbReader mrb;
  ASSERT_TRUE(mrb.open(mrb_path.c_str())) << "MRB open failed";

  printf("  MRB images: %u\n", mrb.image_count());
  ASSERT_GT(mrb.image_count(), 0u) << "alice-illustrated should have images";

  size_t decoded_ok = 0;
  size_t decoded_fail = 0;
  size_t total_white = 0;
  size_t total_black = 0;

  for (uint16_t key = 0; key < mrb.image_count(); ++key) {
    const auto& ref = mrb.image_ref(key);

    // Open the EPUB and read the local entry (no filename � mimics ReaderScreen)
    StdioZipFile file;
    ASSERT_TRUE(file.open(path_.c_str()));
    ZipEntry local_entry;
    auto zerr = ZipReader::read_local_entry(file, ref.local_header_offset, local_entry);
    ASSERT_EQ(zerr, ZipError::Ok) << "read_local_entry failed for image " << key;

    // Collect rows via sink
    struct SinkData {
      std::vector<uint8_t> rows;
      uint16_t width = 0;
      uint16_t height = 0;
    } sink_data;

    ImageRowSink sink;
    sink.ctx = &sink_data;
    sink.emit_row = [](void* ctx, uint16_t y, const uint8_t* data, uint16_t width) {
      auto* sd = static_cast<SinkData*>(ctx);
      if (sd->width == 0)
        sd->width = width;
      size_t stride = (width + 7) / 8;
      size_t needed = static_cast<size_t>(y + 1) * stride;
      if (sd->rows.size() < needed)
        sd->rows.resize(needed, 0);
      std::memcpy(sd->rows.data() + static_cast<size_t>(y) * stride, data, stride);
      if (y + 1 > sd->height)
        sd->height = y + 1;
    };

    DecodedImage dims;
    auto err = decode_image_from_entry(file, local_entry, 440, 700, dims, nullptr, 0, true, &sink);
    if (err != ImageError::Ok) {
      decoded_fail++;
      printf("    image %u: DECODE FAILED (error %d)\n", key, (int)err);
      continue;
    }
    decoded_ok++;

    EXPECT_GT(dims.width, 0) << "image " << key << " decoded to zero width";
    EXPECT_GT(dims.height, 0) << "image " << key << " decoded to zero height";

    // Count white vs black pixels to detect all-black images
    size_t white = 0, black = 0;
    size_t stride = (sink_data.width + 7) / 8;
    for (uint16_t y = 0; y < sink_data.height; ++y) {
      for (uint16_t x = 0; x < sink_data.width; ++x) {
        bool is_white = (sink_data.rows[y * stride + x / 8] >> (7 - (x & 7))) & 1;
        if (is_white)
          white++;
        else
          black++;
      }
    }
    total_white += white;
    total_black += black;

    size_t total = white + black;
    float white_pct = total > 0 ? 100.0f * white / total : 0;
    printf("    image %u: %ux%u  white=%.1f%%\n", key, dims.width, dims.height, white_pct);

    // A natural image should not be 100% black (that's the bug symptom).
    EXPECT_GT(white, 0u) << "image " << key << " is completely black � black squares bug!";
  }

  printf("\n  Decoded: %zu ok, %zu failed\n", decoded_ok, decoded_fail);
  EXPECT_GT(decoded_ok, 0u) << "Expected at least some images to decode";
  EXPECT_EQ(decoded_fail, 0u) << "All image decodes should succeed";

  // Overall: the book's illustrations should have meaningful white content.
  float overall_white = (total_white + total_black) > 0 ? 100.0f * total_white / (total_white + total_black) : 0;
  printf("  Overall white pixel ratio: %.1f%%\n", overall_white);
  EXPECT_GT(overall_white, 10.0f) << "Images are too dark � possible black squares bug";

  // Clean up test MRB
  std::remove(mrb_path.c_str());
  mrb.close();
}

TEST(RegressionTest, SnowCrash) {
  microreader::Book book;
  auto err = book.open("C:/Users/Patrick/Desktop/microreader/microreader2/sd/books/snow crash ger.epub", nullptr,
                       nullptr, false);
  ASSERT_EQ(err, microreader::EpubError::Ok);

  std::printf("Spine size: %zu\n", book.epub().spine().size());
  bool success = microreader::convert_epub_to_mrb_streaming(book, "test_out_snow.mrb");
  EXPECT_TRUE(success);
}
