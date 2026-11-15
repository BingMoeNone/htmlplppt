#pragma once

namespace htmlppt {

class CoverOverlay {
 public:
  virtual ~CoverOverlay() = default;

  virtual bool Show() = 0;
  virtual void Hide() = 0;
  virtual bool SetOpacity(double opacity) = 0;
  virtual void ReassertTopMost() = 0;
};

} // namespace htmlppt
