#pragma once

#include <cstdint>
#include <memory>

#include "core/edge_spec.h"

namespace htmlppt {

class EdgeProcess {
 public:
  virtual ~EdgeProcess() = default;
  virtual std::uint32_t pid() const = 0;
  virtual bool alive() const = 0;
  virtual void terminate() = 0;
  // Drops ownership without terminating the process. Returns false when the
  // platform cannot detach; the controller keeps ownership in that case.
  virtual bool release() = 0;
};

class EdgeLauncher {
 public:
  virtual ~EdgeLauncher() = default;
  virtual std::unique_ptr<EdgeProcess> Launch(const EdgeLaunchSpec& spec) = 0;
};

class EdgeController {
 public:
  explicit EdgeController(EdgeLauncher& launcher);
  ~EdgeController();

  EdgeController(const EdgeController&) = delete;
  EdgeController& operator=(const EdgeController&) = delete;

  bool Launch(const EdgeLaunchSpec& spec);
  void Kill();
  // Detaches the process so it keeps running after this controller is
  // destroyed. Returns false when detaching failed.
  bool Release();
  bool active() const;
  std::uint32_t pid() const;

 private:
  EdgeLauncher& launcher_;
  std::unique_ptr<EdgeProcess> process_;
};

} // namespace htmlppt
