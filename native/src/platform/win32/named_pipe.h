#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/clock.h"
#include "core/supervisor_protocol.h"

namespace htmlppt {

#ifdef _WIN32

// Duplex frame transport shared by both named-pipe ends. The worker owns a
// NamedPipeClient and a test drives a NamedPipeChannel accepted by a server;
// the worker session only needs this interface, so it runs on either end.
class FrameChannel {
 public:
  virtual ~FrameChannel() = default;

  // A failed send leaves the stream misaligned and sets broken().
  virtual bool Send(const Frame& frame) = 0;
  // A nullopt with broken()==false is a clean timeout.
  virtual std::optional<Frame> Receive(Duration timeout) = 0;
  virtual void Close() = 0;
  virtual bool broken() const = 0;
};

// Frame transport over a duplex byte-mode named pipe. Receive(timeout) applies
// the timeout to each underlying read, so a full frame may take longer than
// the requested timeout. A nullopt with broken()==false is a clean timeout;
// a nullopt with broken()==true means the stream is misaligned or closed and
// the caller must close the channel.
class NamedPipeChannel final : public FrameChannel {
 public:
  NamedPipeChannel() = default;
  ~NamedPipeChannel() override;
  NamedPipeChannel(const NamedPipeChannel&) = delete;
  NamedPipeChannel& operator=(const NamedPipeChannel&) = delete;

  // Uses a fixed 3-second timeout. On failure the stream may contain a partial
  // frame and broken() is set, so the channel must be closed.
  bool Send(const Frame& frame) override;
  // See the class comment for the timeout/broken() contract.
  std::optional<Frame> Receive(Duration timeout) override;
  void Close() override;
  bool broken() const override { return broken_; }

 private:
  friend class NamedPipeServer;
  explicit NamedPipeChannel(HANDLE pipe) : pipe_(pipe) {}
  bool ReadExact(std::uint8_t* buffer, std::size_t size, Duration timeout);
  bool WriteExact(const std::uint8_t* buffer, std::size_t size,
                  Duration timeout);
  HANDLE pipe_ = nullptr;
  bool broken_ = false;
};

class NamedPipeServer {
 public:
  NamedPipeServer() = default;
  ~NamedPipeServer();
  NamedPipeServer(const NamedPipeServer&) = delete;
  NamedPipeServer& operator=(const NamedPipeServer&) = delete;

  bool Create(const std::wstring& name);
  // On failure or timeout the server handle is closed; the caller must call
  // Create again before another Accept.
  std::unique_ptr<NamedPipeChannel> Accept(Duration timeout);
  void Close();

 private:
  std::wstring name_;
  HANDLE pipe_ = nullptr;
};

class NamedPipeClient final : public FrameChannel {
 public:
  NamedPipeClient() = default;
  ~NamedPipeClient() override;
  NamedPipeClient(const NamedPipeClient&) = delete;
  NamedPipeClient& operator=(const NamedPipeClient&) = delete;

  bool Connect(const std::wstring& name, Duration timeout);
  // Same fixed 3-second timeout and partial-frame contract as
  // NamedPipeChannel::Send.
  bool Send(const Frame& frame) override;
  // Same timeout and broken() contract as NamedPipeChannel::Receive.
  std::optional<Frame> Receive(Duration timeout) override;
  void Close() override;
  bool broken() const override { return broken_; }

 private:
  bool ReadExact(std::uint8_t* buffer, std::size_t size, Duration timeout);
  bool WriteExact(const std::uint8_t* buffer, std::size_t size,
                  Duration timeout);
  HANDLE pipe_ = nullptr;
  bool broken_ = false;
};

#endif // _WIN32

} // namespace htmlppt
