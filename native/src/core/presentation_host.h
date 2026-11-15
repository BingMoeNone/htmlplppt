#pragma once

#include <filesystem>
#include <string>

namespace htmlppt {

enum class HostStatus {
  Ok,
  Busy,
  Unavailable,
  Failed,
};

struct HostResult {
  HostStatus status = HostStatus::Ok;
  long error = 0;
  std::string detail;

  bool ok() const { return status == HostStatus::Ok; }
};

struct ShowState {
  HostStatus status = HostStatus::Ok;
  bool active = false;
  int slide = 0;
  long error = 0;
  std::string detail;
};

class PresentationHost {
 public:
  virtual ~PresentationHost() = default;

  // Create the presentation application, make it visible, and open the deck
  // read-only. Repeated calls with the same deck are a no-op.
  virtual HostResult Launch(const std::filesystem::path& deck_path) = 0;

  // Start the slideshow from slide 1 and wait until it is active.
  virtual HostResult StartShow() = 0;

  // Poll the slideshow window count and current position.
  virtual ShowState QueryShow() = 0;

  // Leave the slideshow while keeping the deck open.
  virtual HostResult ExitShow() = 0;

  // Ensure the show is running, focus it, and jump to the slide.
  virtual HostResult ResumeShow(int slide) = 0;

  // Focus the slideshow window without changing its position.
  virtual HostResult FocusShow() = 0;

  // Release COM references. Never closes the deck or quits the application.
  virtual void Release() = 0;
};

} // namespace htmlppt
