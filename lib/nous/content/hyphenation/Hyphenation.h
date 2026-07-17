#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "nous/content/Font.h"

namespace microreader {

enum class HyphenationLang {
  None,
  English,     // en
  German,      // de
  French,      // fr
  Spanish,     // es
  Italian,     // it
  Dutch,       // nl
  Portuguese,  // pt
  Polish,      // pl
  Russian,     // ru
};

// Hyphenate a word. Returns the number of valid hyphen positions written to
// out_positions (byte offsets into word). Positions are byte offsets k such
// that a hyphen may be inserted between word[k-1] and word[k].
// leftmin/rightmin are enforced by the Liang algorithm.
int hyphenate_word(const char* word, size_t len, HyphenationLang lang, size_t* out_positions, int max_positions);

// Detect hyphenation language from an IETF language tag (e.g. "de", "de-DE",
// "en", "en-US"). Case-insensitive prefix match. Returns None if unrecognised.
HyphenationLang detect_language(const std::optional<std::string>& lang_tag);

// Find the best byte offset at which to break word_ptr[0..len) so that the
// prefix (plus a synthetic '-' if needed) fits within avail pixels.
// Prefers breaking after an existing '-' in the token; falls back to Liang.
// Returns 0 if no split fits. avail must already exclude any inter-word space.
// out_prefix_has_hyphen is set to true when the prefix already ends with '-'
// (i.e. no synthetic hyphen should be drawn).
size_t find_hyphen_break(const IFont& font, const char* word_ptr, size_t len, FontStyle style, uint8_t size_pct,
                         HyphenationLang lang, uint16_t avail, bool& out_prefix_has_hyphen);

}  // namespace microreader
