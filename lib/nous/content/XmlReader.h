#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace microreader {

// Event types emitted by the XmlReader.
enum class XmlEventType {
  StartElement,
  EndElement,
  Text,
  ProcessingInstruction,
  Comment,
  CData,
  Dtd,
  EndOfFile,
};

// Result codes for XML reading operations.
enum class XmlError {
  Ok = 0,
  Eof,
  ReadError,
  Utf8Error,
  InvalidState,
  BufferTooSmall,
};

// Interface for reading bytes into the XML parser.
class IXmlInput {
 public:
  virtual ~IXmlInput() = default;
  // Read up to `max_size` bytes into `buf`. Return number of bytes read (0 = EOF).
  virtual size_t read(void* buf, size_t max_size) = 0;
};

// A non-owning view into a string. Points into the parser's internal buffer.
// Only valid until the next call to next_event().
struct XmlStringView {
  const char* data = nullptr;
  size_t length = 0;

  bool empty() const { return length == 0; }
  bool operator==(const char* s) const {
    size_t slen = std::strlen(s);
    return slen == length && std::memcmp(data, s, length) == 0;
  }
  bool operator!=(const char* s) const { return !(*this == s); }
};

// Iterator over attributes in an element's attribute string.
// Only valid until the next call to next_event().
struct XmlAttributeReader {
  const char* data = nullptr;
  size_t length = 0;

  // Find an attribute value by name. Returns empty view if not found.
  XmlStringView get(const char* name) const;

  struct Iterator {
    const char* pos;
    const char* end;
    struct Attr {
      XmlStringView name;
      XmlStringView value;
    };
    Attr current;
    bool valid;

    Iterator(const char* data, size_t length);
    bool has_next();
    Attr next();

   private:
    void advance();
    void skip_whitespace();
  };
};

// An XML event. The string views point into the reader's internal buffer
// and are only valid until the next call to next_event().
struct XmlEvent {
  XmlEventType type = XmlEventType::EndOfFile;

  // For StartElement/EndElement/ProcessingInstruction: element/PI name
  XmlStringView name;

  // For StartElement/ProcessingInstruction: attributes
  XmlAttributeReader attrs;

  // For Text/Comment/CData/Dtd: content
  XmlStringView content;
};

// Streaming XML reader. Uses a fixed-size internal buffer.
//
// Usage:
//   MemoryXmlInput input(xml_str, xml_len);
//   XmlReader reader;
//   if (reader.open(input, buf, buf_size) == XmlError::Ok) {
//     XmlEvent event;
//     while (reader.next_event(event) == XmlError::Ok) {
//       if (event.type == XmlEventType::EndOfFile) break;
//       // handle event...
//     }
//   }
class XmlReader {
 public:
  XmlReader() = default;

  // Initialize the reader with an input source and a working buffer.
  // The buffer should be large enough to hold any single element + attributes.
  // 512 bytes is fine for container.xml, 4096+ for content.opf, 8192+ for XHTML.
  XmlError open(IXmlInput& input, uint8_t* buffer, size_t buffer_size);

  // Read the next XML event. Returns Ok on success.
  // When EndOfFile is returned via the event, the stream is done.
  XmlError next_event(XmlEvent& event);

  // Skip past the current element when BufferTooSmall is returned.
  // Scans forward to find '>' then resumes normal parsing.
  // Returns Ok if recovery succeeded, Eof if the stream is exhausted.
  XmlError skip_element();

 private:
  IXmlInput* input_ = nullptr;
  uint8_t* buf_ = nullptr;
  size_t buf_size_ = 0;
  size_t pos_ = 0;    // current read position in buffer
  size_t end_ = 0;    // end of valid data in buffer
  size_t remaining_ = 0; // bytes remaining in the input that haven't been read yet
  bool has_self_closing_ = false;
  // Saved name for self-closing element end event
  const char* self_closing_name_ = nullptr;
  size_t self_closing_name_len_ = 0;

  // Move data starting at `from` to the beginning and refill.
  XmlError advance(size_t from);

  // Ensure at least `min_bytes` are available in the buffer view.
  XmlError ensure(size_t min_bytes);

  // Current buffer view from pos_ to end_.
  const char* view() const { return reinterpret_cast<const char*>(buf_ + pos_); }
  size_t view_len() const { return end_ - pos_; }

  // Find a byte in the current view. Returns offset or SIZE_MAX if not found.
  size_t find_byte(char c) const;

  // Find a needle in the current view. Returns offset or SIZE_MAX if not found.
  size_t find_str(const char* needle, size_t needle_len) const;

  // Parse the block between start/end delimiters as a specific event type.
  XmlError parse_delimited(const char* start_delim, size_t start_len,
                           const char* end_delim, size_t end_len,
                           XmlEvent& event, XmlEventType type);

  // Split "name attrs..." from a block and populate event name + attrs.
  static void split_name_attrs(const char* block, size_t block_len,
                                XmlStringView& name, XmlAttributeReader& attrs);

  // Trim leading ASCII whitespace from a view.
  static void trim_start(const char*& data, size_t& length);
  static void trim_end(const char*& data, size_t& length);
  static void trim(const char*& data, size_t& length);
};

// IXmlInput backed by a memory buffer.
class MemoryXmlInput : public IXmlInput {
 public:
  MemoryXmlInput(const void* data, size_t size) : data_(static_cast<const uint8_t*>(data)), remaining_(size) {}
  size_t read(void* buf, size_t max_size) override;
 private:
  const uint8_t* data_;
  size_t remaining_;
};

}  // namespace microreader
