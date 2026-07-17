#include "Hyphenation.h"

#include <cstdint>
#include <cstring>

#include "Liang/hyph-de.trie.h"
#include "Liang/hyph-en.trie.h"
#include "Liang/hyph-es.trie.h"
#include "Liang/hyph-fr.trie.h"
#include "Liang/hyph-it.trie.h"
#include "Liang/hyph-nl.trie.h"
#include "Liang/hyph-pl.trie.h"
#include "Liang/hyph-pt.trie.h"
#include "Liang/hyph-ru.trie.h"
#include "Liang/liang_hyphenation_patterns.h"

// ---------------------------------------------------------------------------
// Liang hyphenation algorithm using TeX patterns compiled into binary tries
// by Typst hypher: https://github.com/typst/hypher
// ---------------------------------------------------------------------------

static constexpr size_t kMaxWordLen = 128;

// Decoded view of a single trie node pulled from the serialized blob.
// Node layout: [header][?levelsInfo][transitions][targets]
// header bits: 7=hasLevels, 6-5=stride(0→1,else1-3), 4-0=childCount(31=overflow→extraByte)
struct TrieState {
  const HyphenationTrieData* trie;
  size_t addr;
  uint8_t stride;
  uint8_t childCount;
  const uint8_t* transitions;
  const uint8_t* targets;
  const uint8_t* levels;
  uint8_t levelsLen;
};

static TrieState decode_trie_node(const HyphenationTrieData& trie, size_t addr) {
  TrieState s = {};
  if (addr >= trie.size)
    return s;
  const uint8_t* base = trie.data + addr;
  size_t rem = trie.size - addr;
  size_t pos = 0;

  const uint8_t hdr = base[pos++];
  const bool hasLevels = (hdr >> 7) != 0;
  uint8_t stride = (hdr >> 5) & 0x03u;
  if (stride == 0)
    stride = 1;
  size_t childCount = hdr & 0x1Fu;
  if (childCount == 31u) {
    if (pos >= rem)
      return s;
    childCount = base[pos++];
  }

  const uint8_t* levels = nullptr;
  uint8_t levelsLen = 0;
  if (hasLevels) {
    if (pos + 1 >= rem)
      return s;
    const uint8_t hi = base[pos++];
    const uint8_t loLen = base[pos++];
    // 12-bit absolute offset into original blob (before the 4-byte root header was stripped).
    // Subtract 4 to get index into trie.data (which starts at blob byte 4).
    const size_t offset = (static_cast<size_t>(hi) << 4) | (loLen >> 4);
    levelsLen = loLen & 0x0Fu;
    if (offset < 4u || offset + levelsLen > trie.size + 4u)
      return s;
    levels = trie.data + offset - 4u;
  }

  if (pos + childCount > rem)
    return s;
  const uint8_t* transitions = base + pos;
  pos += childCount;
  if (pos + static_cast<size_t>(childCount) * stride > rem)
    return s;
  const uint8_t* targets = base + pos;

  s.trie = &trie;
  s.addr = addr;
  s.stride = stride;
  s.childCount = static_cast<uint8_t>(childCount < 255 ? childCount : 255);
  s.transitions = transitions;
  s.targets = targets;
  s.levels = levels;
  s.levelsLen = levelsLen;
  return s;
}

static int32_t decode_delta(const uint8_t* buf, uint8_t stride) {
  if (stride == 1)
    return static_cast<int8_t>(buf[0]);
  if (stride == 2)
    return static_cast<int16_t>((static_cast<uint16_t>(buf[0]) << 8) | buf[1]);
  const int32_t v = (static_cast<int32_t>(buf[0]) << 16) | (static_cast<int32_t>(buf[1]) << 8) | buf[2];
  return v - (1 << 23);
}

static bool trie_step(const TrieState& state, uint8_t ch, TrieState& out) {
  for (size_t i = 0; i < state.childCount; ++i) {
    if (state.transitions[i] != ch)
      continue;
    const uint8_t* dp = state.targets + i * state.stride;
    const int32_t delta = decode_delta(dp, state.stride);
    const int64_t next = static_cast<int64_t>(state.addr) + delta;
    if (next < 0 || static_cast<size_t>(next) >= state.trie->size)
      return false;
    out = decode_trie_node(*state.trie, static_cast<size_t>(next));
    return out.trie != nullptr;
  }
  return false;
}

static int trie_hyphenate(const char* word, size_t word_len, size_t leftmin, size_t rightmin, size_t* out_positions,
                          int max_positions, const HyphenationTrieData& trie) {
  if (!word || word_len == 0)
    return 0;
  if (word_len > kMaxWordLen)
    word_len = kMaxWordLen;

  // Build augmented word ".word." with simple case-folding:
  // ASCII A-Z → a-z; UTF-8 C3+[80-9E] (Latin-1 uppercase supplement) → C3+[A0-BE].
  uint8_t aug[kMaxWordLen + 3];
  aug[0] = '.';
  for (size_t i = 0; i < word_len; ++i) {
    uint8_t c = static_cast<uint8_t>(word[i]);
    if (c >= 0x41u && c <= 0x5Au) {
      c += 0x20u;  // ASCII uppercase
    } else if (i > 0 && static_cast<uint8_t>(word[i - 1]) == 0xC3u) {
      // UTF-8 continuation byte after C3 prefix — lowercase if in uppercase range
      if (c >= 0x80u && c <= 0x9Eu && c != 0x97u)
        c += 0x20u;
    }
    aug[1 + i] = c;
  }
  const int aug_len = static_cast<int>(word_len) + 2;
  aug[aug_len - 1] = '.';

  // Score array: one byte per augmented position.
  uint8_t scores[kMaxWordLen + 3];
  std::memset(scores, 0, static_cast<size_t>(aug_len));

  // Walk trie from every starting position in the augmented word.
  const TrieState root = decode_trie_node(trie, trie.rootOffset);
  if (!root.trie)
    return 0;

  for (int start = 0; start < aug_len; ++start) {
    TrieState state = root;
    for (int cursor = start; cursor < aug_len; ++cursor) {
      TrieState next;
      if (!trie_step(state, aug[cursor], next))
        break;
      state = next;

      if (state.levels && state.levelsLen > 0) {
        size_t offset = 0;
        for (uint8_t li = 0; li < state.levelsLen; ++li) {
          const uint8_t packed = state.levels[li];
          offset += packed / 10u;
          const uint8_t level = packed % 10u;
          const size_t splitPos = static_cast<size_t>(start) + offset;
          if (splitPos < static_cast<size_t>(aug_len) && level > scores[splitPos])
            scores[splitPos] = level;
        }
      }
    }
  }

  // Emit positions where score is odd and within leftmin/rightmin bounds.
  int count = 0;
  for (size_t k = 1; k <= word_len; ++k) {
    // Score at augmented position k+1 corresponds to a split after byte k-1 in the word.
    // A split at k means: prefix = word[0..k-1], suffix = word[k..end].
    if ((scores[k + 1] & 1u) && k >= leftmin && (word_len - k) >= rightmin) {
      if (count < max_positions)
        out_positions[count] = k;
      ++count;
    }
  }
  return count;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace microreader {

int hyphenate_word(const char* word, size_t /*len*/, HyphenationLang lang, size_t* out_positions, int max_positions) {
  const HyphenationTrieData* trie = nullptr;
  switch (lang) {
    case HyphenationLang::English:
      trie = &en_trie;
      break;
    case HyphenationLang::German:
      trie = &de_trie;
      break;
    case HyphenationLang::French:
      trie = &fr_trie;
      break;
    case HyphenationLang::Spanish:
      trie = &es_trie;
      break;
    case HyphenationLang::Italian:
      trie = &it_trie;
      break;
    case HyphenationLang::Dutch:
      trie = &nl_trie;
      break;
    case HyphenationLang::Portuguese:
      trie = &pt_trie;
      break;
    case HyphenationLang::Polish:
      trie = &pl_trie;
      break;
    case HyphenationLang::Russian:
      trie = &ru_trie;
      break;
    default:
      return 0;
  }
  return trie_hyphenate(word, std::strlen(word), 2, 2, out_positions, max_positions, *trie);
}

size_t find_hyphen_break(const IFont& font, const char* word_ptr, size_t len, FontStyle style, uint8_t size_pct,
                         HyphenationLang lang, uint16_t avail, bool& out_prefix_has_hyphen) {
  out_prefix_has_hyphen = false;
  if (len == 0 || avail == 0)
    return 0;

  // Prefer breaking at an existing '-' in the token (right-most that fits).
  for (size_t i = len - 1; i > 0; --i) {
    if (word_ptr[i - 1] == '-') {
      uint16_t prefix_w = font.word_width(word_ptr, static_cast<uint16_t>(i), style, size_pct);
      if (prefix_w <= avail) {
        out_prefix_has_hyphen = true;
        return i;
      }
    }
  }

  // Fall back to Liang algorithmic hyphenation.
  if (lang == HyphenationLang::None || len < 6)
    return 0;
  char buf[129];
  size_t copy_len = len < 128 ? len : 128;
  memcpy(buf, word_ptr, copy_len);
  buf[copy_len] = '\0';

  // Strip trailing punctuation before hyphenating so the Liang algorithm doesn't produce
  // ugly splits like "befördert-|." where the suffix is just punctuation.
  // Add new entries to either table to extend the set of stripped characters.
  static const char kStripAscii[] = ".,!?:;)]\"'";  // single ASCII bytes to strip
  struct MultiByteSeq {
    unsigned char b0, b1;
  };
  static const MultiByteSeq kStripMulti[] = {
      {0xC2, 0xBB}, // » U+00BB RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK
      {0xC2, 0xAB}, // « U+00AB LEFT-POINTING DOUBLE ANGLE QUOTATION MARK
  };
  size_t hyph_len = copy_len;
  while (hyph_len > 0) {
    bool stripped = false;
    for (const auto& seq : kStripMulti) {
      if (hyph_len >= 2 && (unsigned char)buf[hyph_len - 2] == seq.b0 && (unsigned char)buf[hyph_len - 1] == seq.b1) {
        hyph_len -= 2;
        stripped = true;
        break;
      }
    }
    if (!stripped) {
      unsigned char c = (unsigned char)buf[hyph_len - 1];
      if (c < 0x80 && std::strchr(kStripAscii, c)) {
        --hyph_len;
      } else {
        break;
      }
    }
  }
  buf[hyph_len] = '\0';

  static constexpr int kMinCharsEachSide = 2;

  size_t positions[32];
  int n = hyphenate_word(buf, hyph_len, lang, positions, 32);
  if (n <= 0)
    return 0;
  const uint16_t hyphen_w = font.char_width('-', style, size_pct);

  // Pre-compute cumulative codepoint counts for the word (O(copy_len) once).
  // cum_cp[i] = number of UTF-8 codepoints in word_ptr[0..i).
  uint8_t cum_cp[129];
  cum_cp[0] = 0;
  for (size_t i = 0; i < copy_len && i < 128; ++i)
    cum_cp[i + 1] = cum_cp[i] + (((unsigned char)word_ptr[i] & 0xC0) != 0x80 ? 1 : 0);
  const uint8_t total_hyph_cp = (hyph_len <= copy_len) ? cum_cp[hyph_len] : cum_cp[copy_len];

  // Pre-compute incremental prefix pixel widths (O(copy_len) total instead of O(N * avg_pos)).
  // width(0..positions[i]) ≈ sum of segment widths. Kerning across segment boundaries is not
  // counted, but the error is at most ±2px which is acceptable for hyphenation decisions.
  uint16_t prefix_ws[32];
  {
    size_t prev = 0;
    for (int i = 0; i < n; ++i) {
      size_t pos = positions[i];
      if (pos > copy_len)
        pos = copy_len;
      uint16_t seg =
          (pos > prev) ? font.word_width(word_ptr + prev, static_cast<uint16_t>(pos - prev), style, size_pct) : 0;
      prefix_ws[i] = (i == 0 ? 0 : prefix_ws[i - 1]) + seg;
      prev = pos;
    }
  }

  for (int i = n - 1; i >= 0; --i) {
    size_t pos = positions[i];
    if (pos == 0 || pos >= len)
      continue;
    // Enforce leftmin/rightmin in Unicode code points, not bytes.
    const size_t safe_pos = (pos <= copy_len) ? pos : copy_len;
    if (cum_cp[safe_pos] < kMinCharsEachSide)
      continue;
    if (total_hyph_cp - cum_cp[safe_pos] < kMinCharsEachSide)
      continue;
    uint16_t prefix_w = prefix_ws[i];
    bool ends_with_hyphen = (word_ptr[pos - 1] == '-');
    uint16_t extra = ends_with_hyphen ? 0 : hyphen_w;
    if (prefix_w + extra <= avail) {
      out_prefix_has_hyphen = ends_with_hyphen;
      return pos;
    }
  }
  return 0;
}

HyphenationLang detect_language(const std::optional<std::string>& lang_tag) {
  if (!lang_tag)
    return HyphenationLang::None;

  std::string_view sv = *lang_tag;

  auto ieq = [](std::string_view s, const char* expected) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
      ++i;
    }
    for (size_t j = 0; expected[j]; ++j) {
      if (i >= s.size())
        return false;
      char c = s[i++];
      if (c >= 'A' && c <= 'Z')
        c += 32;
      if (c != expected[j])
        return false;
    }
    if (i < s.size()) {
      char c = s[i];
      if (c != '-' && c != ' ' && c != '\t' && c != '\n' && c != '\r')
        return false;
    }
    return true;
  };

  if (ieq(sv, "de") || ieq(sv, "ger") || ieq(sv, "deu"))
    return HyphenationLang::German;
  if (ieq(sv, "en") || ieq(sv, "eng"))
    return HyphenationLang::English;
  if (ieq(sv, "fr") || ieq(sv, "fra"))
    return HyphenationLang::French;
  if (ieq(sv, "es") || ieq(sv, "spa"))
    return HyphenationLang::Spanish;
  if (ieq(sv, "it") || ieq(sv, "ita"))
    return HyphenationLang::Italian;
  if (ieq(sv, "nl") || ieq(sv, "nld"))
    return HyphenationLang::Dutch;
  if (ieq(sv, "pt") || ieq(sv, "por"))
    return HyphenationLang::Portuguese;
  if (ieq(sv, "pl") || ieq(sv, "pol"))
    return HyphenationLang::Polish;
  if (ieq(sv, "ru") || ieq(sv, "rus"))
    return HyphenationLang::Russian;

  return HyphenationLang::None;
}

}  // namespace microreader
