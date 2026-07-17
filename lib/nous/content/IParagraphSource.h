#pragma once

#include <cstddef>

#include "ContentModel.h"

namespace microreader {

// Abstract interface for accessing paragraphs.
// Lets TextLayout work with in-memory Chapter or on-disk MRB files.
struct IParagraphSource {
  virtual ~IParagraphSource() = default;
  virtual size_t paragraph_count() const = 0;
  virtual const Paragraph& paragraph(size_t index) const = 0;
};

}  // namespace microreader
