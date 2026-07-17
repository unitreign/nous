#include "XmlReader.h"

#include <algorithm>
#include <cstring>

namespace microreader {

// ---------------------------------------------------------------------------
// MemoryXmlInput
// ---------------------------------------------------------------------------

size_t MemoryXmlInput::read(void* buf, size_t max_size) {
  size_t n = std::min(max_size, remaining_);
  if (n > 0) {
    std::memcpy(buf, data_, n);
    data_ += n;
    remaining_ -= n;
  }
  return n;
}

// ---------------------------------------------------------------------------
// XmlAttributeReader
// ---------------------------------------------------------------------------

XmlStringView XmlAttributeReader::get(const char* name) const {
  size_t name_len = std::strlen(name);
  Iterator it(data, length);
  while (it.has_next()) {
    auto attr = it.next();
    if (attr.name.length == name_len && std::memcmp(attr.name.data, name, name_len) == 0) {
      return attr.value;
    }
  }
  return {};
}

XmlAttributeReader::Iterator::Iterator(const char* data, size_t length) : pos(data), end(data + length), valid(false) {}

bool XmlAttributeReader::Iterator::has_next() {
  skip_whitespace();
  return pos < end;
}

XmlAttributeReader::Iterator::Attr XmlAttributeReader::Iterator::next() {
  advance();
  return current;
}

void XmlAttributeReader::Iterator::skip_whitespace() {
  while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')) {
    ++pos;
  }
}

void XmlAttributeReader::Iterator::advance() {
  skip_whitespace();
  current = {};
  if (pos >= end)
    return;

  // Find '='
  const char* eq = static_cast<const char*>(std::memchr(pos, '=', end - pos));
  if (!eq) {
    pos = end;
    return;
  }

  // Name is from pos to eq, trimmed
  current.name.data = pos;
  current.name.length = eq - pos;
  // Trim trailing whitespace from name
  while (current.name.length > 0 &&
         (current.name.data[current.name.length - 1] == ' ' || current.name.data[current.name.length - 1] == '\t')) {
    --current.name.length;
  }

  pos = eq + 1;
  skip_whitespace();
  if (pos >= end)
    return;

  // Value: quoted or unquoted
  char quote = *pos;
  if (quote == '"' || quote == '\'') {
    ++pos;
    const char* close = static_cast<const char*>(std::memchr(pos, quote, end - pos));
    if (close) {
      current.value.data = pos;
      current.value.length = close - pos;
      pos = close + 1;
    } else {
      current.value.data = pos;
      current.value.length = end - pos;
      pos = end;
    }
  } else {
    // Unquoted value — read until whitespace
    current.value.data = pos;
    while (pos < end && *pos != ' ' && *pos != '\t' && *pos != '\n' && *pos != '\r') {
      ++pos;
    }
    current.value.length = pos - current.value.data;
  }
}

// ---------------------------------------------------------------------------
// XmlReader — trim helpers
// ---------------------------------------------------------------------------

void XmlReader::trim_start(const char*& data, size_t& length) {
  while (length > 0 && (*data == ' ' || *data == '\t' || *data == '\n' || *data == '\r')) {
    ++data;
    --length;
  }
}

void XmlReader::trim_end(const char*& data, size_t& length) {
  while (length > 0) {
    char c = data[length - 1];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      --length;
    } else {
      break;
    }
  }
}

void XmlReader::trim(const char*& data, size_t& length) {
  trim_start(data, length);
  trim_end(data, length);
}

// ---------------------------------------------------------------------------
// XmlReader — buffer management
// ---------------------------------------------------------------------------

XmlError XmlReader::open(IXmlInput& input, uint8_t* buffer, size_t buffer_size) {
  input_ = &input;
  buf_ = buffer;
  buf_size_ = buffer_size;
  pos_ = 0;
  end_ = 0;
  remaining_ = SIZE_MAX;  // Unknown total size; we rely on read() returning 0
  has_self_closing_ = false;

  // Initial fill
  size_t n = input_->read(buf_, buf_size_);
  end_ = n;
  // If we got less than buffer_size, the input is exhausted
  if (n < buf_size_) {
    remaining_ = 0;
  }
  return XmlError::Ok;
}

XmlError XmlReader::advance(size_t from) {
  if (remaining_ == 0 && from >= end_) {
    return XmlError::Eof;
  }

  // Move [from..end_) to [0..)
  size_t keep = end_ - from;
  if (keep > 0 && from > 0) {
    std::memmove(buf_, buf_ + from, keep);
  }
  pos_ = 0;
  end_ = keep;

  // Read more data
  if (remaining_ > 0) {
    size_t space = buf_size_ - end_;
    if (space > 0) {
      size_t n = input_->read(buf_ + end_, space);
      end_ += n;
      if (n < space) {
        remaining_ = 0;
      }
    }
  }

  return XmlError::Ok;
}

XmlError XmlReader::ensure(size_t min_bytes) {
  if (view_len() >= min_bytes)
    return XmlError::Ok;
  if (remaining_ == 0)
    return XmlError::Eof;
  return advance(pos_);
}

size_t XmlReader::find_byte(char c) const {
  const void* p = std::memchr(view(), c, view_len());
  if (!p)
    return SIZE_MAX;
  return static_cast<size_t>(static_cast<const char*>(p) - view());
}

size_t XmlReader::find_str(const char* needle, size_t needle_len) const {
  if (needle_len > view_len())
    return SIZE_MAX;
  size_t search_len = view_len() - needle_len + 1;
  for (size_t i = 0; i < search_len; ++i) {
    if (std::memcmp(view() + i, needle, needle_len) == 0) {
      return i;
    }
  }
  return SIZE_MAX;
}

// ---------------------------------------------------------------------------
// XmlReader — event parsing
// ---------------------------------------------------------------------------

void XmlReader::split_name_attrs(const char* block, size_t block_len, XmlStringView& name, XmlAttributeReader& attrs) {
  // Trim the block
  trim(block, block_len);

  // Find first whitespace to separate name from attrs
  size_t i = 0;
  while (i < block_len && block[i] != ' ' && block[i] != '\t' && block[i] != '\n' && block[i] != '\r') {
    ++i;
  }
  name.data = block;
  name.length = i;

  if (i < block_len) {
    attrs.data = block + i;
    attrs.length = block_len - i;
  } else {
    attrs = {};
  }
}

XmlError XmlReader::skip_element() {
  // Called after BufferTooSmall when pos_ points at '<'.
  // Scan forward through the buffer (and the input) until we find '>',
  // then position just past it so the next next_event() call succeeds.
  for (;;) {
    size_t gt = find_byte('>');
    if (gt != SIZE_MAX) {
      pos_ += gt + 1;
      return XmlError::Ok;
    }
    // No '>' in current view — discard everything and read a fresh buffer.
    if (remaining_ == 0) {
      pos_ = end_;
      return XmlError::Eof;
    }
    // Discard the entire current view so advance() can read fresh data.
    XmlError err = advance(end_);
    if (err != XmlError::Ok)
      return err;
    if (view_len() == 0)
      return XmlError::Eof;
  }
}

XmlError XmlReader::next_event(XmlEvent& event) {
  // Handle pending self-closing end element
  if (has_self_closing_) {
    has_self_closing_ = false;
    event.type = XmlEventType::EndElement;
    event.name.data = self_closing_name_;
    event.name.length = self_closing_name_len_;
    event.attrs = {};
    event.content = {};
    return XmlError::Ok;
  }

  // Check for EOF
  if (pos_ >= end_ && remaining_ == 0) {
    event.type = XmlEventType::EndOfFile;
    return XmlError::Ok;
  }

  // Try to find '<'. Everything before it is text content.
  size_t lt_pos = find_byte('<');

  // If not found, try advancing
  if (lt_pos == SIZE_MAX) {
    if (remaining_ > 0) {
      // There's text content before the '<' that we haven't found yet
      // First emit any text we have, then advance
      if (view_len() > 0) {
        event.type = XmlEventType::Text;
        event.content.data = view();
        event.content.length = view_len();
        pos_ = end_;
        return XmlError::Ok;
      }
      XmlError err = advance(pos_);
      if (err != XmlError::Ok) {
        event.type = XmlEventType::EndOfFile;
        return XmlError::Ok;
      }
      lt_pos = find_byte('<');
    }
    if (lt_pos == SIZE_MAX) {
      // Remaining text
      if (view_len() > 0) {
        event.type = XmlEventType::Text;
        event.content.data = view();
        event.content.length = view_len();
        pos_ = end_;
        return XmlError::Ok;
      }
      event.type = XmlEventType::EndOfFile;
      return XmlError::Ok;
    }
  }

  // If there's text before '<', emit it (including whitespace-only text,
  // which may be significant between inline elements like <a>1</a> <a>2</a>).
  if (lt_pos > 0) {
    event.type = XmlEventType::Text;
    event.content.data = view();
    event.content.length = lt_pos;
    pos_ += lt_pos;
    return XmlError::Ok;
  }

  // Now pos_ points at '<'. Ensure we have at least 3 bytes to classify.
  XmlError err = ensure(3);
  if (err != XmlError::Ok) {
    event.type = XmlEventType::EndOfFile;
    return XmlError::Ok;
  }

  const char* v = view();

  // Classify based on bytes after '<'
  if (v[1] == '!' && v[2] == '[') {
    // CDATA: <![CDATA[ ... ]]>
    return parse_delimited("<![CDATA[", 9, "]]>", 3, event, XmlEventType::CData);
  } else if (v[1] == '!' && v[2] == '-') {
    // Comment: <!-- ... -->
    return parse_delimited("<!--", 4, "-->", 3, event, XmlEventType::Comment);
  } else if (v[1] == '!') {
    // DTD: <! ... >
    return parse_delimited("<!", 2, ">", 1, event, XmlEventType::Dtd);
  } else if (v[1] == '?') {
    // Processing instruction: <? ... ?>
    return parse_delimited("<?", 2, "?>", 2, event, XmlEventType::ProcessingInstruction);
  } else if (v[1] == '/') {
    // End element: </name>
    return parse_delimited("</", 2, ">", 1, event, XmlEventType::EndElement);
  } else {
    // Start element: <name attrs...> or <name attrs.../>
    // Find the closing '>'
    size_t gt = find_byte('>');
    if (gt == SIZE_MAX) {
      // Try advancing
      err = advance(pos_);
      if (err != XmlError::Ok) {
        event.type = XmlEventType::EndOfFile;
        return XmlError::Ok;
      }
      gt = find_byte('>');
      if (gt == SIZE_MAX) {
        return XmlError::BufferTooSmall;
      }
    }

    // Block is between '<' and '>'
    const char* block = view() + 1;  // skip '<'
    size_t block_len = gt - 1;       // exclude '>'

    // Check for self-closing '/>'
    bool self_closing = false;
    if (block_len > 0 && block[block_len - 1] == '/') {
      self_closing = true;
      --block_len;
    }

    event.type = XmlEventType::StartElement;
    event.content = {};
    split_name_attrs(block, block_len, event.name, event.attrs);

    if (self_closing) {
      has_self_closing_ = true;
      self_closing_name_ = event.name.data;
      self_closing_name_len_ = event.name.length;
    }

    pos_ += gt + 1;
    return XmlError::Ok;
  }
}

XmlError XmlReader::parse_delimited(const char* start_delim, size_t start_len, const char* end_delim, size_t end_len,
                                    XmlEvent& event, XmlEventType type) {
  // Verify start delimiter — if the view is too short, try to refill first
  if (view_len() < start_len) {
    if (remaining_ == 0)
      return XmlError::InvalidState;
    XmlError err = advance(pos_);
    if (err != XmlError::Ok)
      return XmlError::InvalidState;
  }
  if (view_len() < start_len || std::memcmp(view(), start_delim, start_len) != 0) {
    return XmlError::InvalidState;
  }

  // Find end delimiter starting after start
  size_t search_start = start_len;
  size_t end_pos = SIZE_MAX;

  // Check in current view first
  if (view_len() > search_start) {
    const char* search_from = view() + search_start;
    size_t search_len = view_len() - search_start;
    if (search_len >= end_len) {
      for (size_t i = 0; i <= search_len - end_len; ++i) {
        if (std::memcmp(search_from + i, end_delim, end_len) == 0) {
          end_pos = search_start + i;
          break;
        }
      }
    }
  }

  // If not found, try advancing
  if (end_pos == SIZE_MAX && remaining_ > 0) {
    XmlError err = advance(pos_);
    if (err != XmlError::Ok) {
      event.type = XmlEventType::EndOfFile;
      return XmlError::Ok;
    }
    // Search again
    if (view_len() > start_len) {
      const char* search_from = view() + start_len;
      size_t search_len_local = view_len() - start_len;
      if (search_len_local >= end_len) {
        for (size_t i = 0; i <= search_len_local - end_len; ++i) {
          if (std::memcmp(search_from + i, end_delim, end_len) == 0) {
            end_pos = start_len + i;
            break;
          }
        }
      }
    }
  }

  if (end_pos == SIZE_MAX) {
    return XmlError::BufferTooSmall;
  }

  // Content is between start_delim and end_delim
  const char* content = view() + start_len;
  size_t content_len = end_pos - start_len;

  if (type == XmlEventType::StartElement || type == XmlEventType::ProcessingInstruction) {
    event.type = type;
    event.content = {};
    split_name_attrs(content, content_len, event.name, event.attrs);
  } else if (type == XmlEventType::EndElement) {
    event.type = type;
    event.name.data = content;
    event.name.length = content_len;
    trim(event.name.data, event.name.length);
    event.attrs = {};
    event.content = {};
  } else {
    event.type = type;
    event.name = {};
    event.attrs = {};
    event.content.data = content;
    event.content.length = content_len;
  }

  pos_ += end_pos + end_len;
  return XmlError::Ok;
}

}  // namespace microreader
