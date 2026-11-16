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
};

class EdgeLauncher {
 public:
  virtual ~EdgeLauncher() = default;
  virtual std::unique_ptr<EdgeProcess> Launch(const EdgeLaunchSpec& spec) = 0;
};

class EdgeController {
 public:
  explicit EdgeController(EdgeLauncher& launcher);

  bool Launch(const EdgeLaunchSpec& spec);
  void Kill();
  bool active() const;
  std::uint32_t pid() const;

 private:
  EdgeLauncher& launcher_;
  std::unique_ptr<EdgeProcess> process_;
};

} // namespace htmlppt
