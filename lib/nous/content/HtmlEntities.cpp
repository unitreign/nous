#include "HtmlEntities.h"

#include <cctype>

namespace microreader {

void append_utf8(std::string& out, uint32_t code) {
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xC0 | (code >> 6));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    out += static_cast<char>(0xE0 | (code >> 12));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code >> 18));
    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  }
}

DecodedEntity decode_html_entity(const char* entity, size_t entity_len) {
  struct NamedEntity {
    std::string_view name;
    std::string_view utf8;
  };

  static constexpr NamedEntity kNamedEntities[] = {
      {"amp",    "&" },
      {"lt",     "<" },
      {"gt",     ">" },
      {"quot",   "\""},
      {"apos",   "'" },
      {"nbsp",   " " },
      {"ensp",   " " },
      {"emsp",   " " },
      {"thinsp", " " },
      {"iexcl",  "¡" },
      {"cent",   "¢" },
      {"pound",  "£" },
      {"curren", "¤" },
      {"yen",    "¥" },
      {"brvbar", "¦" },
      {"sect",   "§" },
      {"uml",    "¨" },
      {"copy",   "©" },
      {"ordf",   "ª" },
      {"laquo",  "«" },
      {"not",    "¬" },
      {"shy",    "­" },
      {"reg",    "®" },
      {"macr",   "¯" },
      {"deg",    "°" },
      {"plusmn", "±" },
      {"sup2",   "²" },
      {"sup3",   "³" },
      {"acute",  "´" },
      {"micro",  "µ" },
      {"para",   "¶" },
      {"middot", "·" },
      {"cedil",  "¸" },
      {"sup1",   "¹" },
      {"ordm",   "º" },
      {"raquo",  "»" },
      {"frac14", "¼" },
      {"frac12", "½" },
      {"frac34", "¾" },
      {"iquest", "¿" },
      {"Agrave", "À" },
      {"Aacute", "Á" },
      {"Acirc",  "Â" },
      {"Atilde", "Ã" },
      {"Auml",   "Ä" },
      {"Aring",  "Å" },
      {"AElig",  "Æ" },
      {"Ccedil", "Ç" },
      {"Egrave", "È" },
      {"Eacute", "É" },
      {"Ecirc",  "Ê" },
      {"Euml",   "Ë" },
      {"Igrave", "Ì" },
      {"Iacute", "Í" },
      {"Icirc",  "Î" },
      {"Iuml",   "Ï" },
      {"ETH",    "Ð" },
      {"Ntilde", "Ñ" },
      {"Ograve", "Ò" },
      {"Oacute", "Ó" },
      {"Ocirc",  "Ô" },
      {"Otilde", "Õ" },
      {"Ouml",   "Ö" },
      {"times",  "×" },
      {"Oslash", "Ø" },
      {"Ugrave", "Ù" },
      {"Uacute", "Ú" },
      {"Ucirc",  "Û" },
      {"Uuml",   "Ü" },
      {"Yacute", "Ý" },
      {"THORN",  "Þ" },
      {"szlig",  "ß" },
      {"agrave", "à" },
      {"aacute", "á" },
      {"acirc",  "â" },
      {"atilde", "ã" },
      {"auml",   "ä" },
      {"aring",  "å" },
      {"aelig",  "æ" },
      {"ccedil", "ç" },
      {"egrave", "è" },
      {"eacute", "é" },
      {"ecirc",  "ê" },
      {"euml",   "ë" },
      {"igrave", "ì" },
      {"iacute", "í" },
      {"icirc",  "î" },
      {"iuml",   "ï" },
      {"eth",    "ð" },
      {"ntilde", "ñ" },
      {"ograve", "ò" },
      {"oacute", "ó" },
      {"ocirc",  "ô" },
      {"otilde", "õ" },
      {"ouml",   "ö" },
      {"divide", "÷" },
      {"oslash", "ø" },
      {"ugrave", "ù" },
      {"uacute", "ú" },
      {"ucirc",  "û" },
      {"uuml",   "ü" },
      {"yacute", "ý" },
      {"thorn",  "þ" },
      {"yuml",   "ÿ" },
      {"OElig",  "Œ" },
      {"oelig",  "œ" },
      {"Scaron", "Š" },
      {"scaron", "š" },
      {"Yuml",   "Ÿ" },
      {"fnof",   "ƒ" },
      {"circ",   "ˆ" },
      {"tilde",  "˜" },
      {"ndash",  "–" },
      {"mdash",  "—" },
      {"lsquo",  "‘" },
      {"rsquo",  "’" },
      {"sbquo",  "‚" },
      {"ldquo",  "“" },
      {"rdquo",  "”" },
      {"bdquo",  "„" },
      {"dagger", "†" },
      {"Dagger", "‡" },
      {"bull",   "•" },
      {"hellip", "…" },
      {"permil", "‰" },
      {"prime",  "′" },
      {"Prime",  "″" },
      {"lsaquo", "‹" },
      {"rsaquo", "›" },
      {"oline",  "‾" },
      {"frasl",  "⁄" },
      {"euro",   "€" },
      {"trade",  "™" },
  };

  std::string_view ent(entity, entity_len);
  for (const auto& named : kNamedEntities) {
    if (named.name == ent) {
      return {(named.utf8 == " ") ? DecodedEntity::Kind::Space : DecodedEntity::Kind::Utf8, named.utf8, 0};
    }
  }

  if (!ent.empty() && ent.front() == '#') {
    uint32_t code = 0;
    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
      for (size_t j = 2; j < ent.size(); ++j) {
        unsigned char c = static_cast<unsigned char>(ent[j]);
        if (!std::isxdigit(c))
          return {};
        code = code * 16 + (std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10);
      }
    } else {
      for (size_t j = 1; j < ent.size(); ++j) {
        unsigned char c = static_cast<unsigned char>(ent[j]);
        if (!std::isdigit(c))
          return {};
        code = code * 10 + (c - '0');
      }
    }
    if (code == 0xA0 || code == ' ' || code == '\t' || code == '\n' || code == '\r')
      return {DecodedEntity::Kind::Space, {}, 0};
    return {DecodedEntity::Kind::Codepoint, {}, code};
  }

  return {};
}

}  // namespace microreader
