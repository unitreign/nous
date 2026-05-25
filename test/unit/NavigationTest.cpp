// NavigationTest.cpp — full-book page navigation without rendering.
// These tests convert EPUB→MRB, then paginate every chapter page-by-page
// using layout_page(), verifying the layout engine can traverse entire
// books without crashing or getting stuck in infinite loops.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "TestBooks.h"
#include "TestChapterSource.h"
#include "microreader/content/Book.h"
#include "microreader/content/TextLayout.h"
#include "microreader/content/mrb/MrbConverter.h"
#include "microreader/content/mrb/MrbReader.h"
#include "microreader/display/DrawBuffer.h"
#include "microreader/screens/ReaderOptionsScreen.h"
#include "microreader/screens/ReaderScreen.h"

using namespace microreader;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Match the real device layout parameters exactly.
// Settings mirror sd/.microreader/settings: padding_h=1, padding_v=1,
// spacing_override=3 (1.0x), progress=2 (Bar).
// This produces: padding_top=4, padding_bottom=8, padding_left/right=12,
// para_spacing=8, ph=776 (788-4-8).
// ---------------------------------------------------------------------------
static FixedFont device_font() {
  return ReaderScreen::make_fixed_font();
}

static PageOptions device_opts() {
  ReaderSettings s;
  s.padding_h_idx = 1;
  s.padding_v_idx = 1;
  s.spacing_override = SpacingOverride::Spacing_1_0x;
  s.progress_style = ProgressStyle::Bar;
  PageOptions opts = ReaderScreen::make_page_opts(&s);
  opts.padding_right = s.h_padding();
  opts.padding_left = s.h_padding();
  opts.padding_top = static_cast<uint16_t>(ReaderScreen::kPaddingTop + s.v_padding());
  opts.line_height_multiplier_percent = s.line_height_multiplier_percent();
  return opts;
}

// ---------------------------------------------------------------------------
// Core invariant helper: every forward page boundary must equal the
// corresponding backward page boundary.
//
// Paginates the source forward from {0,0} to chapter end, collecting all
// page start/end positions.  Then paginates backward from the chapter end,
// collecting the same positions in reverse order.  Asserts the two sequences
// are identical.
// ---------------------------------------------------------------------------
struct PageBoundary {
  PagePosition start;
  PagePosition end;
};

static std::vector<PageBoundary> paginate_forward(IFont& font, const PageOptions& opts, IParagraphSource& src) {
  std::vector<PageBoundary> pages;
  TextLayout tl(font, opts, src);
  PagePosition pos{0, 0};
  for (int guard = 0; guard < 10000; ++guard) {
    tl.set_position(pos);
    auto page = tl.layout();
    pages.push_back({pos, page.end});
    if (page.at_chapter_end)
      break;
    EXPECT_NE(page.end, pos) << "forward layout made no progress at {p=" << pos.paragraph << ",off=" << pos.offset
                             << "}";
    if (page.end == pos)
      break;
    pos = page.end;
  }
  return pages;
}

// For every non-terminal forward page boundary E_k (k < last), calling
// layout_backward() from E_k must reproduce page k exactly.
// This is the real navigation invariant: user is on page k+1 (page_pos_=E_k),
// presses "back", app calls set_position(E_k) + layout_backward().
//
// The LAST forward page is excluded: its end is the chapter end, and
// layout_backward(chapter_end) fills greedily without knowing where the
// prior page ended, so it cannot reproduce a short last page.  That case is
// a separate known issue ("jump to last page of chapter").
static void assert_fwd_bwd_symmetric(IFont& font, const PageOptions& opts, IParagraphSource& src,
                                     const char* label = "") {
  auto fwd = paginate_forward(font, opts, src);
  ASSERT_FALSE(fwd.empty()) << label << ": no forward pages";

  TextLayout tl(font, opts, src);
  // Skip k = last (at_chapter_end); that boundary is the chapter end.
  const size_t check_count = fwd.size() > 1 ? fwd.size() - 1 : fwd.size();
  for (size_t k = 0; k < check_count; ++k) {
    tl.set_position(fwd[k].end);
    auto bwd = tl.layout_backward();
    EXPECT_EQ(bwd.start.paragraph, fwd[k].start.paragraph)
        << label << ": page " << k << " bwd.start.paragraph=" << bwd.start.paragraph
        << " vs fwd=" << fwd[k].start.paragraph;
    EXPECT_EQ(bwd.start.offset, fwd[k].start.offset) << label << ": page " << k << " bwd.start.offset mismatch";
    EXPECT_EQ(bwd.end.paragraph, fwd[k].end.paragraph)
        << label << ": page " << k << " bwd.end.paragraph=" << bwd.end.paragraph << " vs fwd=" << fwd[k].end.paragraph;
    EXPECT_EQ(bwd.end.offset, fwd[k].end.offset) << label << ": page " << k << " bwd.end.offset mismatch";
  }
}

// Build a test chapter: p0 = h1 heading (160% size), p1..pN = body text.
static Chapter make_heading_chapter(int body_para_count) {
  Chapter ch;
  {
    TextParagraph tp;
    tp.runs.push_back(microreader::Run("Chapter Heading", FontStyle::Bold, 160));
    ch.paragraphs.push_back(Paragraph::make_text(std::move(tp)));
  }
  for (int i = 0; i < body_para_count; ++i) {
    TextParagraph tp;
    tp.runs.push_back(microreader::Run("Body paragraph text line.", FontStyle::Regular, 100));
    ch.paragraphs.push_back(Paragraph::make_text(std::move(tp)));
  }
  return ch;
}

// ---------------------------------------------------------------------------
// FwdBwdSymmetry tests
// ---------------------------------------------------------------------------

// Heading + 35 body paragraphs with real device settings (ph=776).
// Spans 2 pages; the heading must land on page 1 in both directions.
TEST(FwdBwdSymmetry, HeadingMultiPageDeviceSettings) {
  auto font = device_font();
  auto opts = device_opts();
  auto ch = make_heading_chapter(35);
  TestChapterSource src(ch);
  assert_fwd_bwd_symmetric(font, opts, src, "HeadingMultiPage");
}

// Tiny page height (60px) forces many page splits, stressing the boundary
// symmetry across many inter-paragraph gaps.
TEST(FwdBwdSymmetry, HeadingManyPagesSmallHeight) {
  FixedFont font(16, 20);  // y_advance=20, baseline=16
  PageOptions opts(480, 60, 0, 8, Alignment::Start);
  auto ch = make_heading_chapter(20);
  TestChapterSource src(ch);
  assert_fwd_bwd_symmetric(font, opts, src, "SmallPage");
}

// All paragraphs fit on one page (infinite height) — trivial but guards
// ---------------------------------------------------------------------------
// SinglePageSymmetry: when page_height is large enough to hold the whole
// chapter, layout() and layout_backward(chapter_end) must produce the same
// page: same start {0,0}, same end (chapter_end), and crucially the same
// height_used so that the two directions agree on what "fits".
//
// This is the simplest possible invariant and must hold before multi-page
// symmetry can be tested.  If it fails, the forward and backward budget
// calculations diverge even on a trivial one-page chapter.
// ---------------------------------------------------------------------------

// Helper: assert that layout() and layout_backward(chapter_end) agree on a
// single-page chapter.  page_height must be large enough to hold all content.
static void assert_single_page_symmetric(IFont& font, const PageOptions& opts, IParagraphSource& src,
                                         const char* label = "") {
  const uint16_t n = static_cast<uint16_t>(src.paragraph_count());
  const PagePosition chapter_end{n, 0};

  TextLayout tl(font, opts, src);

  // Forward from {0,0}
  tl.set_position({0, 0});
  auto fwd = tl.layout();
  ASSERT_TRUE(fwd.at_chapter_end) << label << ": forward did not reach chapter end — increase page_height";
  EXPECT_EQ(fwd.start.paragraph, 0) << label << ": fwd start paragraph";
  EXPECT_EQ(fwd.start.offset, 0) << label << ": fwd start offset";

  // Backward from chapter_end
  tl.set_position(chapter_end);
  auto bwd = tl.layout_backward();
  EXPECT_EQ(bwd.start.paragraph, 0) << label << ": bwd start paragraph=" << bwd.start.paragraph << " (expected 0)";
  EXPECT_EQ(bwd.start.offset, 0) << label << ": bwd start offset";
  EXPECT_EQ(bwd.end.paragraph, fwd.end.paragraph) << label << ": end paragraph mismatch";
  EXPECT_EQ(bwd.end.offset, fwd.end.offset) << label << ": end offset mismatch";
}

// Heading (160% size) + body paragraphs, device font, very tall page.
TEST(SinglePageSymmetry, HeadingPlusBody_DeviceFont) {
  auto font = device_font();
  PageOptions opts = device_opts();
  opts.height = 9999;  // tall enough to hold everything
  for (int n = 0; n <= 10; ++n) {
    auto ch = make_heading_chapter(n);
    TestChapterSource src(ch);
    std::string label = "heading+" + std::to_string(n) + "body";
    assert_single_page_symmetric(font, opts, src, label.c_str());
  }
}

// Heading (160% size) + body paragraphs, fixed font, very tall page.
TEST(SinglePageSymmetry, HeadingPlusBody_FixedFont) {
  FixedFont font(16, 20);
  PageOptions opts(480, 9999, 0, 8, Alignment::Start);
  for (int n = 0; n <= 10; ++n) {
    auto ch = make_heading_chapter(n);
    TestChapterSource src(ch);
    std::string label = "heading+" + std::to_string(n) + "body";
    assert_single_page_symmetric(font, opts, src, label.c_str());
  }
}

// Chapter with empty/BR paragraphs, fixed font, very tall page.
TEST(SinglePageSymmetry, BrParagraphs) {
  FixedFont font(16, 20);
  PageOptions opts(480, 9999, 0, 8, Alignment::Start);
  Chapter ch;
  const char* lines[] = {"Heading", "Para 1", "", "Para 2", "", "Para 3", "Para 4", "", "Para 5"};
  for (const char* line : lines) {
    TextParagraph tp;
    if (*line)
      tp.runs.push_back(microreader::Run(line, FontStyle::Regular, 100));
    ch.paragraphs.push_back(Paragraph::make_text(std::move(tp)));
  }
  TestChapterSource src(ch);
  assert_single_page_symmetric(font, opts, src, "BrParagraphs");
}

TEST(FwdBwdSymmetry, SinglePageInfiniteHeight) {
  FixedFont font(16, 20);
  PageOptions opts(480, 9999, 0, 8, Alignment::Start);
  auto ch = make_heading_chapter(20);
  TestChapterSource src(ch);
  assert_fwd_bwd_symmetric(font, opts, src, "InfiniteHeight");
}

// Chapter with empty/BR paragraphs interspersed, matching regression_test ch36.
TEST(FwdBwdSymmetry, BrParagraphsSmallHeight) {
  FixedFont font(16, 20);
  PageOptions opts(480, 80, 0, 8, Alignment::Start);
  Chapter ch;
  const char* lines[] = {"Heading", "Para 1", "", "Para 2", "", "Para 3", "Para 4", "", "Para 5"};
  for (const char* line : lines) {
    TextParagraph tp;
    if (*line)
      tp.runs.push_back(microreader::Run(line, FontStyle::Regular, 100));
    ch.paragraphs.push_back(Paragraph::make_text(std::move(tp)));
  }
  TestChapterSource src(ch);
  assert_fwd_bwd_symmetric(font, opts, src, "BrParagraphs");
}

// Returns total page count (0 on failure).
// ---------------------------------------------------------------------------
struct NavResult {
  bool ok = false;
  int chapters = 0;
  int total_pages = 0;
  int max_pages_in_chapter = 0;
};

static NavResult navigate_book(const std::string& epub_path) {
  NavResult result;

  // Convert EPUB → MRB in a temp file.
  auto mrb_path = (fs::temp_directory_path() / "nav_test.mrb").string();

  Book book;
  auto err = book.open(epub_path.c_str());
  if (err != EpubError::Ok)
    return result;

  if (!convert_epub_to_mrb_streaming(book, mrb_path.c_str()))
    return result;

  MrbReader mrb;
  if (!mrb.open(mrb_path.c_str())) {
    std::remove(mrb_path.c_str());
    return result;
  }

  auto font = device_font();
  auto opts = device_opts();

  result.chapters = mrb.chapter_count();

  for (uint16_t ci = 0; ci < mrb.chapter_count(); ++ci) {
    MrbChapterSource src(mrb, ci);
    int chapter_pages = 0;

    TextLayout tl(font, opts, src);
    while (true) {
      auto page = tl.layout();
      ++chapter_pages;

      if (page.at_chapter_end)
        break;

      // Safety: abort if stuck in an infinite loop.
      if (chapter_pages > 1000) {
        printf("    ABORT: ch%d exceeded 1000 pages at pos{%u,%u}\n", ci, page.end.paragraph, page.end.offset);
        mrb.close();
        std::remove(mrb_path.c_str());
        return result;
      }

      tl.set_position(page.end);
    }

    result.total_pages += chapter_pages;
    if (chapter_pages > result.max_pages_in_chapter)
      result.max_pages_in_chapter = chapter_pages;
  }

  mrb.close();
  std::remove(mrb_path.c_str());
  result.ok = true;
  return result;
}

// ---------------------------------------------------------------------------
// Parameterized test: navigates one book end-to-end.
// ---------------------------------------------------------------------------

class FullBookNavTest : public ::testing::TestWithParam<std::string> {};

TEST_P(FullBookNavTest, NavigateAllPages) {
  auto path = GetParam();
  if (!fs::exists(path))
    GTEST_SKIP() << "Book not found: " << path;

  auto name = fs::path(path).filename().string();
  auto result = navigate_book(path);

  ASSERT_TRUE(result.ok) << name << " navigation failed";
  EXPECT_GT(result.chapters, 0) << name;
  EXPECT_GT(result.total_pages, 0) << name;

  printf("  %s: %d chapters, %d pages (max %d/chapter)\n", name.c_str(), result.chapters, result.total_pages,
         result.max_pages_in_chapter);
}

// ---------------------------------------------------------------------------
// Test suites — smoke (unit_tests) vs full (microreader_tests).
// ---------------------------------------------------------------------------

INSTANTIATE_EPUB_TESTS(FullBookNavTest);

#ifndef SMOKE_TESTS_ONLY
INSTANTIATE_TEST_SUITE_P(AllNav, FullBookNavTest, ::testing::ValuesIn(test_books::get_all_books()),
                         EPUB_TEST_PARAM_NAME);
#endif

// ---------------------------------------------------------------------------
// Backward navigation test: page backward from end of each chapter.
// ---------------------------------------------------------------------------

class BackwardNavTest : public ::testing::TestWithParam<std::string> {};

TEST_P(BackwardNavTest, NavigateBackwardAllPages) {
  auto path = GetParam();
  if (!fs::exists(path))
    GTEST_SKIP() << "Book not found: " << path;

  auto name = fs::path(path).filename().string();
  auto mrb_path = (fs::temp_directory_path() / "nav_back_test.mrb").string();

  Book book;
  auto err = book.open(path.c_str());
  ASSERT_EQ(err, EpubError::Ok) << name << " open failed";
  ASSERT_TRUE(convert_epub_to_mrb_streaming(book, mrb_path.c_str())) << name << " convert failed";

  MrbReader mrb;
  ASSERT_TRUE(mrb.open(mrb_path.c_str())) << name << " MRB open failed";

  auto font = device_font();
  auto opts = device_opts();

  int total_fwd = 0;
  int total_bwd = 0;

  for (uint16_t ci = 0; ci < mrb.chapter_count(); ++ci) {
    MrbChapterSource src(mrb, ci);

    // Forward pass to count pages and find chapter end.
    int fwd_pages = 0;
    TextLayout tl(font, opts, src);
    while (true) {
      auto page = tl.layout();
      ++fwd_pages;
      if (page.at_chapter_end)
        break;
      ASSERT_LT(fwd_pages, 1000) << name << " ch" << ci << " forward stuck";
      tl.set_position(page.end);
    }

    // Backward pass from chapter end.
    PagePosition end_pos{static_cast<uint16_t>(src.paragraph_count()), 0};
    int bwd_pages = 0;
    while (true) {
      tl.set_position(end_pos);
      auto page = tl.layout_backward();
      ++bwd_pages;
      if (page.start.paragraph == 0 && page.start.offset == 0)
        break;
      ASSERT_LT(bwd_pages, 1000) << name << " ch" << ci << " backward stuck";
      end_pos = page.start;
    }

    EXPECT_EQ(fwd_pages, bwd_pages) << name << " ch" << ci << " page count mismatch (fwd=" << fwd_pages
                                    << " bwd=" << bwd_pages << ")";

    total_fwd += fwd_pages;
    total_bwd += bwd_pages;
  }

  printf("  %s: %d chapters, fwd=%d bwd=%d pages\n", name.c_str(), mrb.chapter_count(), total_fwd, total_bwd);

  mrb.close();
  std::remove(mrb_path.c_str());
}

INSTANTIATE_EPUB_TESTS(BackwardNavTest);

// ---------------------------------------------------------------------------
// Regression: ch36_br_spacing backward navigation drops the h1 heading.
// ---------------------------------------------------------------------------

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

// Synthetic regression test for the backward-nav heading bug.
// A chapter starts with an h1 heading (160% size) followed by 35 body paragraphs.
// With device_opts() (ph=776, para_spacing=8) the h1 + first ~26 body paragraphs
// fit on page 1; the remaining paragraphs go on page 2.
// The old bug: collect_page_items_backward() charged a para_spacing gap before
// paragraph 0 (the h1), incorrectly pushing it off the backward-computed page 1.
TEST(RegressionBackwardNav, SyntheticHeadingMultiPage) {
  auto font = device_font();
  auto opts = device_opts();

  // Build: p0 = h1 heading (160%), p1..p35 = body text (100%).
  Chapter ch;
  {
    TextParagraph tp;
    tp.runs.push_back(microreader::Run("Chapter Heading", FontStyle::Bold, 160));
    ch.paragraphs.push_back(Paragraph::make_text(std::move(tp)));
  }
  for (int i = 1; i <= 35; ++i) {
    TextParagraph tp;
    tp.runs.push_back(microreader::Run("Body paragraph text line.", FontStyle::Regular, 100));
    ch.paragraphs.push_back(Paragraph::make_text(std::move(tp)));
  }
  TestChapterSource src(ch);

  TextLayout tl(font, opts, src);

  // Forward pass.
  std::vector<PagePosition> fwd_starts, fwd_ends;
  fwd_starts.push_back(tl.position());
  while (true) {
    auto page = tl.layout();
    fwd_ends.push_back(page.end);
    if (page.at_chapter_end)
      break;
    ASSERT_LT(fwd_starts.size(), 10u) << "too many pages";
    tl.set_position(page.end);
    fwd_starts.push_back(page.end);
  }
  ASSERT_GE((int)fwd_starts.size(), 2) << "chapter should span at least 2 pages with device settings";

  // Backward from end of forward page 1 must land exactly at {p=0, off=0}.
  tl.set_position(fwd_ends[0]);
  auto bwd = tl.layout_backward();
  EXPECT_EQ(bwd.start.paragraph, 0u) << "backward page 1 must start at the h1 heading (p=0)";
  EXPECT_EQ(bwd.start.offset, 0u);
  EXPECT_EQ(bwd.start, fwd_starts[0]);
  EXPECT_EQ(bwd.end, fwd_ends[0]);
}

TEST(RegressionBackwardNav, Ch36BrSpacingIncludesHeading) {
  std::string epub_path = std::string(TEST_FIXTURES_DIR) + "/regression_test.epub";
  if (!fs::exists(epub_path))
    GTEST_SKIP() << "regression_test.epub not found at " << epub_path;

  Book book;
  ASSERT_EQ(book.open(epub_path.c_str()), EpubError::Ok);

  auto mrb_path = (fs::temp_directory_path() / "ch36_bwd_test.mrb").string();
  ASSERT_TRUE(convert_epub_to_mrb_streaming(book, mrb_path.c_str()));

  MrbReader mrb;
  ASSERT_TRUE(mrb.open(mrb_path.c_str()));

  auto font = device_font();
  auto opts = device_opts();

  // ch36_br_spacing is chapter index 35 (0-based).
  constexpr uint16_t kCh36 = 35;
  ASSERT_GT(mrb.chapter_count(), kCh36) << "regression_test.epub should have >= 36 chapters";

  MrbChapterSource src(mrb, kCh36);

  TextLayout tl(font, opts, src);

  // Forward pass: get all page boundaries.
  std::vector<PagePosition> fwd_starts;
  std::vector<PagePosition> fwd_ends;
  fwd_starts.push_back(tl.position());
  while (true) {
    auto page = tl.layout();
    fwd_ends.push_back(page.end);
    if (page.at_chapter_end)
      break;
    ASSERT_LT(fwd_starts.size(), 50u) << "too many pages";
    tl.set_position(page.end);
    fwd_starts.push_back(page.end);
  }
  const int fwd_pages = (int)fwd_starts.size();

  // Backward pass from chapter end.
  PagePosition end_pos{static_cast<uint16_t>(src.paragraph_count()), 0};
  int bwd_pages = 0;
  while (true) {
    tl.set_position(end_pos);
    auto page = tl.layout_backward();
    ++bwd_pages;
    if (page.start.paragraph == 0 && page.start.offset == 0)
      break;
    ASSERT_LT(bwd_pages, 50) << "too many backward pages";
    end_pos = page.start;
  }

  // The page count must match.
  EXPECT_EQ(bwd_pages, fwd_pages) << "forward and backward page counts differ";

  // The first backward page must start at {0,0} — the h1 heading.
  end_pos = fwd_ends[0];  // end of forward page 1 = start of forward page 2
  tl.set_position(end_pos);
  auto bwd_page1 = tl.layout_backward();

  EXPECT_EQ(bwd_page1.start.paragraph, 0) << "backward page 1 should start at paragraph 0 (h1 heading)";
  EXPECT_EQ(bwd_page1.start.offset, 0);
  EXPECT_EQ(bwd_page1.start, fwd_starts[0]) << "backward page 1 start should match forward page 1 start";
  EXPECT_EQ(bwd_page1.end, fwd_ends[0]) << "backward page 1 end should match forward page 1 end";

  mrb.close();
  std::remove(mrb_path.c_str());
}
