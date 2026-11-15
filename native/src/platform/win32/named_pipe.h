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

// Frame transport over a duplex byte-mode named pipe. Receive(timeout) applies
// the timeout to each underlying read, so a full frame may take longer than
// the requested timeout. A nullopt result leaves the byte stream potentially
// misaligned; the caller must close the channel after any nullopt.
class NamedPipeChannel {
 public:
  NamedPipeChannel() = default;
  ~NamedPipeChannel();
  NamedPipeChannel(const NamedPipeChannel&) = delete;
  NamedPipeChannel& operator=(const NamedPipeChannel&) = delete;

  // Uses a fixed 3-second timeout. On failure the stream may contain a partial
  // frame, so the channel must be closed.
  bool Send(const Frame& frame);
  std::optional<Frame> Receive(Duration timeout);
  void Close();

 private:
  friend class NamedPipeServer;
  explicit NamedPipeChannel(HANDLE pipe) : pipe_(pipe) {}
  bool ReadExact(std::uint8_t* buffer, std::size_t size, Duration timeout);
  bool WriteExact(const std::uint8_t* buffer, std::size_t size,
                  Duration timeout);
  HANDLE pipe_ = nullptr;
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

class NamedPipeClient {
 public:
  NamedPipeClient() = default;
  ~NamedPipeClient();
  NamedPipeClient(const NamedPipeClient&) = delete;
  NamedPipeClient& operator=(const NamedPipeClient&) = delete;

  bool Connect(const std::wstring& name, Duration timeout);
  // Same fixed 3-second timeout and partial-frame contract as
  // NamedPipeChannel::Send.
  bool Send(const Frame& frame);
  // Same timeout and nullopt contract as NamedPipeChannel::Receive.
  std::optional<Frame> Receive(Duration timeout);
  void Close();

 private:
  bool ReadExact(std::uint8_t* buffer, std::size_t size, Duration timeout);
  bool WriteExact(const std::uint8_t* buffer, std::size_t size,
                  Duration timeout);
  HANDLE pipe_ = nullptr;
};

#endif // _WIN32

} // namespace htmlppt
