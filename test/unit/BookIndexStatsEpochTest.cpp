// BookIndexStatsEpochTest.cpp — the one-time reset of poisoned reading times.
//
// Background: update_read_time() used to do `e.read_time_ms += ms` while the
// caller passed an already-cumulative total, so stored times grew quadratically
// with every close. The `+=` was fixed, but no migration ever ran, so the
// inflated values are still sitting in book_index.dat. The stats epoch in the
// file header clears them without disturbing the book list.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "nous/content/BookIndex.h"

namespace fs = std::filesystem;
using namespace microreader;

class BookIndexStatsEpochTest : public ::testing::Test {
 protected:
  fs::path temp_dir_;
  std::string index_path_;

  void SetUp() override {
    temp_dir_ = fs::temp_directory_path() /
                ("nous_idx_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::create_directories(temp_dir_);
    index_path_ = (temp_dir_ / "book_index.dat").string();
    BookIndex::instance().clear_entries();
  }

  void TearDown() override {
    BookIndex::instance().clear_entries();
    std::error_code ec;
    fs::remove_all(temp_dir_, ec);
  }

  void write_raw(const std::string& content) {
    std::ofstream f(index_path_, std::ios::binary);
    f << content;
  }

  std::string read_raw() {
    std::ifstream f(index_path_, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
};

// A file written before the fix has no epoch marker. Its reading times are
// unusable and must be dropped — but the library itself must survive.
TEST_F(BookIndexStatsEpochTest, PreEpochFileLosesReadTimesButKeepsBooks) {
  write_raw(
      "#microreader-index v2\n"
      "/books/a.epub|Book A|Author A|5|31260000\n"   // the reported 8h41m
      "/books/b.epub|Book B|Author B|3|20760000\n"); // and 5h46m

  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  const auto& entries = BookIndex::instance().entries();
  ASSERT_EQ(entries.size(), 2u);

  const auto& pool = BookIndex::instance().pool();
  EXPECT_EQ(entries[0].path.view(pool), "/books/a.epub");
  EXPECT_EQ(entries[0].title.view(pool), "Book A");
  EXPECT_EQ(entries[0].author.view(pool), "Author A");
  EXPECT_EQ(entries[0].last_open_order, 5u) << "open ordering must be preserved";
  EXPECT_EQ(entries[0].read_time_ms, 0u) << "poisoned reading time must be cleared";
  EXPECT_EQ(entries[1].last_open_order, 3u);
  EXPECT_EQ(entries[1].read_time_ms, 0u);
}

// Once the file carries the current epoch, times are trusted and round-trip.
TEST_F(BookIndexStatsEpochTest, CurrentEpochRoundTrips) {
  ASSERT_TRUE(BookIndex::instance().add_entry("/books/a.epub", "Book A", "Author A", 7, 90000));
  ASSERT_TRUE(BookIndex::instance().save(index_path_));

  BookIndex::instance().clear_entries();
  ASSERT_TRUE(BookIndex::instance().load(index_path_));

  const auto& entries = BookIndex::instance().entries();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].last_open_order, 7u);
  EXPECT_EQ(entries[0].read_time_ms, 90000u);
}

TEST_F(BookIndexStatsEpochTest, SaveWritesTheEpochMarker) {
  ASSERT_TRUE(BookIndex::instance().add_entry("/books/a.epub", "T", "A", 1, 1000));
  ASSERT_TRUE(BookIndex::instance().save(index_path_));

  const std::string raw = read_raw();
  EXPECT_NE(raw.find("#microreader-index v2 e1"), std::string::npos) << raw;
}

// The epoch is read from after the version digits — "#microreader-index"
// itself contains an 'e', which a naive scan would latch onto.
TEST_F(BookIndexStatsEpochTest, EpochParsedFromHeaderNotFromTheWordMicroreader) {
  write_raw(
      "#microreader-index v2 e1\n"
      "/books/a.epub|Book A|Author A|4|123456\n");

  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  ASSERT_EQ(BookIndex::instance().entries().size(), 1u);
  EXPECT_EQ(BookIndex::instance().entries()[0].read_time_ms, 123456u)
      << "a correctly-stamped file must keep its reading time";
}

// A stale epoch (some future downgrade, or a partially migrated card) is
// treated the same as no epoch at all.
TEST_F(BookIndexStatsEpochTest, MismatchedEpochAlsoClears) {
  write_raw(
      "#microreader-index v2 e0\n"
      "/books/a.epub|Book A|Author A|4|123456\n");

  ASSERT_TRUE(BookIndex::instance().load(index_path_));
  ASSERT_EQ(BookIndex::instance().entries().size(), 1u);
  EXPECT_EQ(BookIndex::instance().entries()[0].read_time_ms, 0u);
  EXPECT_EQ(BookIndex::instance().entries()[0].last_open_order, 4u);
}

// update_read_time assigns; it must never accumulate. This is the regression
// that produced the quadratic growth in the first place.
TEST_F(BookIndexStatsEpochTest, UpdateReadTimeAssignsRatherThanAccumulates) {
  ASSERT_TRUE(BookIndex::instance().add_entry("/books/a.epub", "T", "A", 1, 0));

  BookIndex::instance().update_read_time("/books/a.epub", 60000, index_path_);
  BookIndex::instance().update_read_time("/books/a.epub", 120000, index_path_);
  BookIndex::instance().update_read_time("/books/a.epub", 180000, index_path_);

  ASSERT_EQ(BookIndex::instance().entries().size(), 1u);
  EXPECT_EQ(BookIndex::instance().entries()[0].read_time_ms, 180000u)
      << "three closes of a growing cumulative total must not sum to 360000";
}
