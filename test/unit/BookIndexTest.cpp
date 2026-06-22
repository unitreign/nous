// BookIndexTest.cpp — unit tests for incremental book index mutations.
//
// These tests cover the bugs fixed in the incremental-reindex rework:
//   B1: Web Manager upload (CMND 'W') didn't update the index.
//   B2: serial_cmd upload via index_file truncated the .dat when entries_ was empty.
//   B3: Multiple uploads before menu load lost all but the last.
//   B4: Delete (CMND 'R') truncated the .dat + had a race with the main loop.
//   B5: Rename (CMND 'N') never updated the index.
//
// All mutations are validated against the "entries_ empty in memory" condition
// (the state left by MainMenu::stop()) to verify ensure_loaded_ semantics.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "ScreenshotDisplay.h"
#include "TestBooks.h"
#include "microreader/content/BookIndex.h"
#include "microreader/display/DrawBuffer.h"

namespace fs = std::filesystem;
using namespace microreader;

namespace {

void write_file(const fs::path& p, const std::string& content) {
  std::ofstream f(p);
  f << content;
}

}  // namespace

class BookIndexTest : public ::testing::Test {
 protected:
  fs::path temp_dir_;
  std::string index_path_;
  ScreenshotDisplay display_;
  DrawBuffer buf_{display_};

  void SetUp() override {
    BookIndex::instance().clear_entries();
    temp_dir_ = fs::temp_directory_path() / "bookindex_test";
    fs::create_directories(temp_dir_);
    index_path_ = (temp_dir_ / "book_index.dat").string();
    std::error_code ec;
    fs::remove(index_path_, ec);
  }

  void TearDown() override {
    BookIndex::instance().clear_entries();
    std::error_code ec;
    fs::remove_all(temp_dir_, ec);
  }

  // Returns the path of a real EPUB for tests that need Book::open.
  std::string smoke_book() const {
    auto books = test_books::get_smoke_books();
    return books.empty() ? std::string{} : books.front();
  }

  // Look up an entry by path string; returns nullptr if not found.
  const BookIndexEntry* find_by_path(const std::string& path) const {
    const auto& pool = BookIndex::instance().pool();
    for (const auto& e : BookIndex::instance().entries()) {
      if (e.path.view(pool) == path)
        return &e;
    }
    return nullptr;
  }
};

// ---------------------------------------------------------------------------
// is_book_path — mirrors the filter in build_index (BookIndex.cpp:186-194).
// ---------------------------------------------------------------------------

TEST_F(BookIndexTest, IsBookPath_AcceptsSdcardBooks) {
  EXPECT_TRUE(BookIndex::is_book_path("/sdcard/books/alice.epub"));
}

TEST_F(BookIndexTest, IsBookPath_AcceptsAnySdcardFolder) {
  EXPECT_TRUE(BookIndex::is_book_path("/sdcard/random/x.epub"));
  EXPECT_TRUE(BookIndex::is_book_path("/sdcard/a/b/c/deep.epub"));
  EXPECT_TRUE(BookIndex::is_book_path("/sdcard/root.epub"));
}

TEST_F(BookIndexTest, IsBookPath_AcceptsCaseInsensitiveExt) {
  EXPECT_TRUE(BookIndex::is_book_path("/sdcard/books/alice.EPUB"));
  EXPECT_TRUE(BookIndex::is_book_path("/sdcard/books/alice.Epub"));
  EXPECT_TRUE(BookIndex::is_book_path("/sdcard/books/alice.ePuB"));
}

TEST_F(BookIndexTest, IsBookPath_RejectsOutsideSdcard) {
  EXPECT_FALSE(BookIndex::is_book_path("/flash/a.epub"));
  EXPECT_FALSE(BookIndex::is_book_path("/spiffs/x.epub"));
  EXPECT_FALSE(BookIndex::is_book_path("relative.epub"));
  EXPECT_FALSE(BookIndex::is_book_path(""));
}

TEST_F(BookIndexTest, IsBookPath_RejectsNonEpub) {
  EXPECT_FALSE(BookIndex::is_book_path("/sdcard/books/alice.txt"));
  EXPECT_FALSE(BookIndex::is_book_path("/sdcard/books/alice.mrb"));
  EXPECT_FALSE(BookIndex::is_book_path("/sdcard/books/alice.png"));
  EXPECT_FALSE(BookIndex::is_book_path("/sdcard/books/alice"));
}

TEST_F(BookIndexTest, IsBookPath_RejectsDotEpubOnly) {
  // Path whose filename is just ".epub" (len <= 5); matches build_index's filter.
  EXPECT_FALSE(BookIndex::is_book_path("/sdcard/books/.epub"));
}

// ---------------------------------------------------------------------------
// index_file — B2/B3 regression: must not truncate .dat when entries_ empty.
// ---------------------------------------------------------------------------

TEST_F(BookIndexTest, IndexFile_PreservesExisting) {
  auto book = smoke_book();
  if (book.empty()) GTEST_SKIP() << "No test EPUB available";

  write_file(index_path_,
             "/sdcard/books/a.epub|Title A|Author A|0\n"
             "/sdcard/books/b.epub|Title B|Author B|0\n"
             "/sdcard/books/c.epub|Title C|Author C|0\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  ASSERT_EQ(BookIndex::instance().entries().size(), 3u);

  // Simulate MainMenu::stop() clearing in-memory state.
  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().entries().empty());

  // This call would TRUNCATE the .dat to 1 entry without ensure_loaded_.
  ASSERT_TRUE(BookIndex::instance().index_file(book, index_path_, buf_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 4u);

  // Verify persistence from disk.
  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 4u);
}

TEST_F(BookIndexTest, IndexFile_DedupSamePath) {
  auto book = smoke_book();
  if (book.empty()) GTEST_SKIP() << "No test EPUB available";

  ASSERT_TRUE(BookIndex::instance().index_file(book, index_path_, buf_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 1u);

  // Indexing the same path again must not duplicate.
  ASSERT_TRUE(BookIndex::instance().index_file(book, index_path_, buf_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 1u);

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 1u);
}

TEST_F(BookIndexTest, IndexFile_PreservesOtherOrders) {
  auto book = smoke_book();
  if (book.empty()) GTEST_SKIP() << "No test EPUB available";

  write_file(index_path_,
             "/sdcard/books/a.epub|Title A|Author A|5\n"
             "/sdcard/books/b.epub|Title B|Author B|3\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  BookIndex::instance().clear_entries();

  ASSERT_TRUE(BookIndex::instance().index_file(book, index_path_, buf_));

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));

  const auto* a = find_by_path("/sdcard/books/a.epub");
  const auto* b = find_by_path("/sdcard/books/b.epub");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->last_open_order, 5u);
  EXPECT_EQ(b->last_open_order, 3u);
}

TEST_F(BookIndexTest, IndexFile_ResetsOrderForReuploadedSamePath) {
  // Option C: re-uploading same path = treat as new book (order reset to 0).
  auto book = smoke_book();
  if (book.empty()) GTEST_SKIP() << "No test EPUB available";

  ASSERT_TRUE(BookIndex::instance().index_file(book, index_path_, buf_));
  BookIndex::instance().set_last_opened(book, 42);
  ASSERT_TRUE(BookIndex::instance().save(index_path_));

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().index_file(book, index_path_, buf_));

  ASSERT_EQ(BookIndex::instance().entries().size(), 1u);
  EXPECT_EQ(BookIndex::instance().entries()[0].last_open_order, 0u);
}

TEST_F(BookIndexTest, IndexFile_CreatesFileIfMissing) {
  auto book = smoke_book();
  if (book.empty()) GTEST_SKIP() << "No test EPUB available";

  ASSERT_FALSE(fs::exists(index_path_));
  ASSERT_TRUE(BookIndex::instance().index_file(book, index_path_, buf_));
  EXPECT_TRUE(fs::exists(index_path_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 1u);
}

TEST_F(BookIndexTest, IndexFile_FailsOnInvalidBook) {
  // A non-EPUB file: Book::open should fail, .dat must remain untouched.
  fs::path junk = temp_dir_ / "junk.epub";
  write_file(junk, "not a real epub");

  write_file(index_path_, "/sdcard/books/existing.epub|T|A|0\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  BookIndex::instance().clear_entries();

  EXPECT_FALSE(BookIndex::instance().index_file(junk.string(), index_path_, buf_));

  // Index unchanged on disk.
  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 1u);
}

// ---------------------------------------------------------------------------
// remove_path — B4 regression.
// ---------------------------------------------------------------------------

TEST_F(BookIndexTest, RemovePath_PreservesOthers) {
  write_file(index_path_,
             "/sdcard/books/a.epub|A|Au|0\n"
             "/sdcard/books/b.epub|B|Bu|0\n"
             "/sdcard/books/c.epub|C|Cu|0\n"
             "/sdcard/books/d.epub|D|Du|0\n"
             "/sdcard/books/e.epub|E|Eu|0\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  BookIndex::instance().clear_entries();  // simulate MainMenu::stop

  EXPECT_TRUE(BookIndex::instance().remove_path("/sdcard/books/c.epub", index_path_));

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 4u);
  EXPECT_EQ(find_by_path("/sdcard/books/c.epub"), nullptr);
}

TEST_F(BookIndexTest, RemovePath_NoOpIfMissing) {
  write_file(index_path_, "/sdcard/books/a.epub|A|Au|0\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  BookIndex::instance().clear_entries();

  EXPECT_TRUE(BookIndex::instance().remove_path("/sdcard/books/nonexistent.epub", index_path_));

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 1u);
}

TEST_F(BookIndexTest, RemovePath_PreservesOtherOrders) {
  write_file(index_path_,
             "/sdcard/books/a.epub|A|Au|5\n"
             "/sdcard/books/b.epub|B|Bu|3\n"
             "/sdcard/books/c.epub|C|Cu|1\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  BookIndex::instance().clear_entries();

  EXPECT_TRUE(BookIndex::instance().remove_path("/sdcard/books/b.epub", index_path_));

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  const auto* a = find_by_path("/sdcard/books/a.epub");
  const auto* c = find_by_path("/sdcard/books/c.epub");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(a->last_open_order, 5u);
  EXPECT_EQ(c->last_open_order, 1u);
}

// ---------------------------------------------------------------------------
// rename_in_place — B5 regression.
// ---------------------------------------------------------------------------

TEST_F(BookIndexTest, RenameInPlace_UpdatesEntry) {
  write_file(index_path_, "/sdcard/books/a.epub|Title A|Author A|5\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  BookIndex::instance().clear_entries();

  EXPECT_TRUE(BookIndex::instance().rename_in_place(
      "/sdcard/books/a.epub", "/sdcard/books/b.epub", index_path_));

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  ASSERT_EQ(BookIndex::instance().entries().size(), 1u);
  const auto& e = BookIndex::instance().entries()[0];
  EXPECT_EQ(e.path.view(BookIndex::instance().pool()), "/sdcard/books/b.epub");
  EXPECT_EQ(e.title.view(BookIndex::instance().pool()), "Title A");
  EXPECT_EQ(e.author.view(BookIndex::instance().pool()), "Author A");
  EXPECT_EQ(e.last_open_order, 5u);
}

TEST_F(BookIndexTest, RenameInPlace_ReturnsFalseIfSrcMissing) {
  write_file(index_path_, "/sdcard/books/a.epub|A|Au|0\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  BookIndex::instance().clear_entries();

  EXPECT_FALSE(BookIndex::instance().rename_in_place(
      "/sdcard/books/nonexistent.epub", "/sdcard/books/x.epub", index_path_));

  // File untouched.
  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 1u);
  EXPECT_NE(find_by_path("/sdcard/books/a.epub"), nullptr);
}

// ---------------------------------------------------------------------------
// build_index — preserves last_open_order via title+author matching.
// ---------------------------------------------------------------------------

TEST_F(BookIndexTest, BuildIndex_PreservesLastOpenedOrder) {
  auto book = smoke_book();
  if (book.empty()) GTEST_SKIP() << "No test EPUB available";

  // Place a real EPUB in a temp "books" dir so build_index finds it.
  fs::path books_dir = temp_dir_ / "books";
  fs::create_directories(books_dir);
  fs::copy_file(book, books_dir / "smoke.epub", fs::copy_options::overwrite_existing);

  // First index + assign an order so we have something to preserve.
  ASSERT_TRUE(BookIndex::instance().index_file(book, index_path_, buf_));
  BookIndex::instance().set_last_opened(book, 7);
  ASSERT_TRUE(BookIndex::instance().save(index_path_));

  // Clear + reload so build_index has old_orders to match against.
  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));

  BookIndex::instance().build_index(books_dir.string(), buf_);

  ASSERT_EQ(BookIndex::instance().entries().size(), 1u);
  EXPECT_EQ(BookIndex::instance().entries()[0].last_open_order, 7u);
}

// ---------------------------------------------------------------------------
// Load/save round-trip + parser robustness.
// ---------------------------------------------------------------------------

TEST_F(BookIndexTest, LoadSave_RoundTrip) {
  BookIndex::instance().add_entry("/sdcard/a.epub", "Title", "Author", 42);
  ASSERT_TRUE(BookIndex::instance().save(index_path_));

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  ASSERT_EQ(BookIndex::instance().entries().size(), 1u);
  const auto& e = BookIndex::instance().entries()[0];
  EXPECT_EQ(e.path.view(BookIndex::instance().pool()), "/sdcard/a.epub");
  EXPECT_EQ(e.title.view(BookIndex::instance().pool()), "Title");
  EXPECT_EQ(e.author.view(BookIndex::instance().pool()), "Author");
  EXPECT_EQ(e.last_open_order, 42u);
}

TEST_F(BookIndexTest, Load_MalformedSkipsBadLines) {
  write_file(index_path_,
             "/sdcard/books/a.epub|A|Au|0\n"
             "garbage line without pipes\n"
             "/sdcard/books/b.epub|B|Bu|0\n"
             "|missing_path\n"
             "/sdcard/books/c.epub|C|Cu|5\n");
  EXPECT_TRUE(BookIndex::instance().load(index_path_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 3u);
}

TEST_F(BookIndexTest, LoadSave_MetadataWithPipeIsTruncatedOnReload) {
  // Documented limitation: the .dat parser splits on '|', so titles containing
  // '|' are mangled on reload. This test pins the current behavior so we notice
  // if it ever changes.
  BookIndex::instance().add_entry("/sdcard/a.epub", "Hello | World", "Author", 0);
  ASSERT_TRUE(BookIndex::instance().save(index_path_));

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  ASSERT_EQ(BookIndex::instance().entries().size(), 1u);
  // "Hello | World|Author|0" parses as title="Hello ", author=" World".
  EXPECT_EQ(BookIndex::instance().entries()[0].title.view(BookIndex::instance().pool()), "Hello ");
  EXPECT_EQ(BookIndex::instance().entries()[0].author.view(BookIndex::instance().pool()), " World");
}

// ---------------------------------------------------------------------------
// Generation counter — used by MainMenu to detect external mutations.
// ---------------------------------------------------------------------------

TEST_F(BookIndexTest, Generation_StartsAtZeroAfterClear) {
  BookIndex::instance().clear_entries();
  EXPECT_EQ(BookIndex::instance().generation(), 0u);
}

TEST_F(BookIndexTest, Generation_BumpsOnRemovePath) {
  write_file(index_path_, "/sdcard/a.epub|A|Au|0\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  const uint64_t g0 = BookIndex::instance().generation();

  EXPECT_TRUE(BookIndex::instance().remove_path("/sdcard/a.epub", index_path_));
  EXPECT_GT(BookIndex::instance().generation(), g0);
}

TEST_F(BookIndexTest, Generation_BumpsOnRenameInPlace) {
  write_file(index_path_, "/sdcard/a.epub|A|Au|0\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  const uint64_t g0 = BookIndex::instance().generation();

  EXPECT_TRUE(BookIndex::instance().rename_in_place(
      "/sdcard/a.epub", "/sdcard/b.epub", index_path_));
  EXPECT_GT(BookIndex::instance().generation(), g0);
}

TEST_F(BookIndexTest, Generation_BumpsOnIndexFile) {
  auto book = smoke_book();
  if (book.empty()) GTEST_SKIP() << "No test EPUB available";

  const uint64_t g0 = BookIndex::instance().generation();
  ASSERT_TRUE(BookIndex::instance().index_file(book, index_path_, buf_));
  EXPECT_GT(BookIndex::instance().generation(), g0);
}

TEST_F(BookIndexTest, Generation_NotBumpedByLoadOrClear) {
  write_file(index_path_, "/sdcard/a.epub|T|A|0\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  const uint64_t g_after_load = BookIndex::instance().generation();

  BookIndex::instance().clear_entries();
  EXPECT_EQ(BookIndex::instance().generation(), g_after_load);
}

// ---------------------------------------------------------------------------
// Sequence of mixed operations — final state must be consistent.
// ---------------------------------------------------------------------------

TEST_F(BookIndexTest, ConcurrentOps_SequenceConsistent) {
  auto book = smoke_book();
  if (book.empty()) GTEST_SKIP() << "No test EPUB available";

  write_file(index_path_,
             "/sdcard/books/seed1.epub|Seed1|Au1|0\n"
             "/sdcard/books/seed2.epub|Seed2|Au2|0\n");
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  BookIndex::instance().clear_entries();

  // 1. Add real book.
  ASSERT_TRUE(BookIndex::instance().index_file(book, index_path_, buf_));
  // 2. Rename seed1.
  EXPECT_TRUE(BookIndex::instance().rename_in_place(
      "/sdcard/books/seed1.epub", "/sdcard/books/seed1_renamed.epub", index_path_));
  // 3. Remove seed2.
  EXPECT_TRUE(BookIndex::instance().remove_path("/sdcard/books/seed2.epub", index_path_));

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  EXPECT_EQ(BookIndex::instance().entries().size(), 2u);
  EXPECT_NE(find_by_path("/sdcard/books/seed1_renamed.epub"), nullptr);
  EXPECT_EQ(find_by_path("/sdcard/books/seed1.epub"), nullptr);
  EXPECT_EQ(find_by_path("/sdcard/books/seed2.epub"), nullptr);
}
