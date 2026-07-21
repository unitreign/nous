#pragma once

#include <string>
#include <vector>

#include "../display/DrawBuffer.h"
#include "IScreen.h"

namespace microreader {

// Batch-converts all un-converted EPUBs in the book index to .mrb format.
// Conversions run one per update() call (synchronous, ~seconds each).
// Pressing back while converting requests cancellation; the current book
// finishes, then the screen stops. Already-converted books are skipped.
class ConvertAllScreen final : public IScreen {
 public:
  ConvertAllScreen() = default;

  const char* name() const override { return "ConvertAll"; }

  void start(DrawBuffer& buf, IRuntime& runtime) override;
  void stop() override;
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 private:
  DrawBuffer* buf_ = nullptr;

  enum class Phase { Converting, Covers, Done };
  Phase phase_ = Phase::Converting;

  struct BookJob {
    std::string path;
    std::string title;
    std::string mrb_path;
    bool done = false;
    bool failed = false;
    bool skipped = false;
  };

  std::vector<BookJob> jobs_;
  int current_idx_ = 0;
  int cover_idx_ = 0;
  int converted_count_ = 0;
  int failed_count_ = 0;
  bool cancel_requested_ = false;

  static std::string derive_stem_(const std::string& path);
  void scan_jobs_();
  void draw_done_(DrawBuffer& buf) const;
};

}  // namespace microreader
