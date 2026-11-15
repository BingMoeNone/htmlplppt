#include "platform/win32/named_pipe.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace htmlppt {
namespace {

constexpr DWORD kPipeBufferSize = 64 * 1024;

bool WaitFor(HANDLE event, Duration timeout) {
  const auto count = timeout.count();
  const DWORD wait_ms = count <= 0 ? 0 : static_cast<DWORD>(count);
  return WaitForSingleObject(event, wait_ms) == WAIT_OBJECT_0;
}

std::uint32_t ReadPayloadLength(const std::uint8_t* header) {
  return static_cast<std::uint32_t>(header[22]) |
         (static_cast<std::uint32_t>(header[23]) << 8) |
         (static_cast<std::uint32_t>(header[24]) << 16) |
         (static_cast<std::uint32_t>(header[25]) << 24);
}

enum class IoStatus { Ok, Timeout, Error };

// A timeout is clean only when no bytes of this read were completed. A timeout
// after a partial read, a Win32 error, or a zero-byte read after data started
// leaves the stream misaligned.
IoStatus ReadPipeExact(HANDLE pipe, std::uint8_t* buffer, std::size_t size,
                       Duration timeout, std::size_t* transferred) {
  std::size_t offset = 0;
  while (offset < size) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
      *transferred = offset;
      return IoStatus::Error;
    }
    DWORD read = 0;
    const BOOL started =
        ReadFile(pipe, buffer + offset, static_cast<DWORD>(size - offset),
                 nullptr, &overlapped);
    bool ok = started != FALSE;
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
      CloseHandle(overlapped.hEvent);
      *transferred = offset;
      return IoStatus::Error;
    }
    if (!ok) {
      ok = WaitFor(overlapped.hEvent, timeout) != FALSE;
      if (!ok) {
        CancelIoEx(pipe, &overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
        CloseHandle(overlapped.hEvent);
        *transferred = offset;
        return offset == 0 ? IoStatus::Timeout : IoStatus::Error;
      }
      ok = GetOverlappedResult(pipe, &overlapped, &read, FALSE) != FALSE;
    } else {
      ok = GetOverlappedResult(pipe, &overlapped, &read, FALSE) != FALSE;
    }
    CloseHandle(overlapped.hEvent);
    if (!ok) {
      *transferred = offset;
      return IoStatus::Error;
    }
    if (read == 0) {
      *transferred = offset;
      return offset == 0 ? IoStatus::Timeout : IoStatus::Error;
    }
    offset += read;
  }
  *transferred = offset;
  return IoStatus::Ok;
}

bool WritePipeExact(HANDLE pipe, const std::uint8_t* buffer, std::size_t size,
                    Duration timeout) {
  std::size_t offset = 0;
  while (offset < size) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return false;
    DWORD written = 0;
    const BOOL started =
        WriteFile(pipe, buffer + offset, static_cast<DWORD>(size - offset),
                  nullptr, &overlapped);
    bool ok = started != FALSE;
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
      CloseHandle(overlapped.hEvent);
      return false;
    }
    if (!ok) {
      ok = WaitFor(overlapped.hEvent, timeout) != FALSE;
      if (!ok) {
        CancelIoEx(pipe, &overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
        CloseHandle(overlapped.hEvent);
        return false;
      }
      ok = GetOverlappedResult(pipe, &overlapped, &written, FALSE) != FALSE;
    } else {
      ok = GetOverlappedResult(pipe, &overlapped, &written, FALSE) != FALSE;
    }
    CloseHandle(overlapped.hEvent);
    if (!ok || written == 0) return false;
    offset += written;
  }
  return true;
}

enum class ReadStatus { Frame, Timeout, Error };

ReadStatus ReceiveFrame(HANDLE pipe, Duration timeout, Frame* frame) {
  std::uint8_t header[kFrameHeaderSize] = {};
  std::size_t transferred = 0;
  const auto header_status =
      ReadPipeExact(pipe, header, sizeof(header), timeout, &transferred);
  if (header_status == IoStatus::Timeout) return ReadStatus::Timeout;
  if (header_status == IoStatus::Error) return ReadStatus::Error;
  const std::uint32_t length = ReadPayloadLength(header);
  if (length > kMaxFramePayload) return ReadStatus::Error;
  std::vector<std::uint8_t> bytes(kFrameHeaderSize + length, 0);
  std::copy(header, header + kFrameHeaderSize, bytes.begin());
  if (length > 0) {
    const auto payload_status = ReadPipeExact(
        pipe, bytes.data() + kFrameHeaderSize, length, timeout, &transferred);
    // A failed payload read, including a timeout after a partial read, leaves
    // the byte stream misaligned.
    if (payload_status != IoStatus::Ok) return ReadStatus::Error;
  }
  const auto decoded = DecodeFrame(bytes);
  if (decoded.status != DecodeStatus::Ok) return ReadStatus::Error;
  *frame = decoded.frame;
  return ReadStatus::Frame;
}

} // namespace

NamedPipeChannel::~NamedPipeChannel() { Close(); }

void NamedPipeChannel::Close() {
  if (pipe_ != nullptr) {
    DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
    pipe_ = nullptr;
  }
}

bool NamedPipeChannel::ReadExact(std::uint8_t* buffer, std::size_t size,
                                 Duration timeout) {
  std::size_t transferred = 0;
  return ReadPipeExact(pipe_, buffer, size, timeout, &transferred) ==
         IoStatus::Ok;
}

bool NamedPipeChannel::WriteExact(const std::uint8_t* buffer, std::size_t size,
                                  Duration timeout) {
  return WritePipeExact(pipe_, buffer, size, timeout);
}

bool NamedPipeChannel::Send(const Frame& frame) {
  const auto bytes = EncodeFrame(frame);
  if (bytes.empty()) {
    broken_ = true;
    return false;
  }
  if (!WriteExact(bytes.data(), bytes.size(), Duration{3000})) {
    broken_ = true;
    return false;
  }
  return true;
}

std::optional<Frame> NamedPipeChannel::Receive(Duration timeout) {
  if (pipe_ == nullptr) {
    broken_ = true;
    return std::nullopt;
  }
  Frame frame;
  const auto status = ReceiveFrame(pipe_, timeout, &frame);
  if (status == ReadStatus::Frame) return frame;
  if (status == ReadStatus::Error) broken_ = true;
  return std::nullopt;
}

NamedPipeServer::~NamedPipeServer() { Close(); }

void NamedPipeServer::Close() {
  if (pipe_ != nullptr) {
    CloseHandle(pipe_);
    pipe_ = nullptr;
  }
  name_.clear();
}

bool NamedPipeServer::Create(const std::wstring& name) {
  Close();
  pipe_ = CreateNamedPipeW(
      name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, kPipeBufferSize,
      kPipeBufferSize, 0, nullptr);
  if (pipe_ == INVALID_HANDLE_VALUE) {
    pipe_ = nullptr;
    return false;
  }
  name_ = name;
  return true;
}

std::unique_ptr<NamedPipeChannel> NamedPipeServer::Accept(Duration timeout) {
  if (pipe_ == nullptr) return nullptr;
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (overlapped.hEvent == nullptr) return nullptr;
  BOOL connected = ConnectNamedPipe(pipe_, &overlapped);
  bool ok = connected != FALSE;
  if (!ok) {
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
      ok = true;
    } else if (error == ERROR_IO_PENDING) {
      ok = WaitFor(overlapped.hEvent, timeout) != FALSE;
      if (!ok) {
        CancelIoEx(pipe_, &overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe_, &overlapped, &ignored, TRUE);
        CloseHandle(overlapped.hEvent);
        Close();
        return nullptr;
      }
      DWORD transferred = 0;
      ok = GetOverlappedResult(pipe_, &overlapped, &transferred, FALSE) != FALSE;
    } else {
      CloseHandle(overlapped.hEvent);
      Close();
      return nullptr;
    }
  }
  CloseHandle(overlapped.hEvent);
  if (!ok) {
    Close();
    return nullptr;
  }
  HANDLE accepted = pipe_;
  pipe_ = nullptr;
  return std::unique_ptr<NamedPipeChannel>(new NamedPipeChannel(accepted));
}

NamedPipeClient::~NamedPipeClient() { Close(); }

void NamedPipeClient::Close() {
  if (pipe_ != nullptr) {
    CloseHandle(pipe_);
    pipe_ = nullptr;
  }
}

bool NamedPipeClient::Connect(const std::wstring& name, Duration timeout) {
  Close();
  broken_ = false;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout.count());
  for (;;) {
    pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    const DWORD error = GetLastError();
    pipe_ = nullptr;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    if (error == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(name.c_str(), 50);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

bool NamedPipeClient::ReadExact(std::uint8_t* buffer, std::size_t size,
                                Duration timeout) {
  std::size_t transferred = 0;
  return ReadPipeExact(pipe_, buffer, size, timeout, &transferred) ==
         IoStatus::Ok;
}

bool NamedPipeClient::WriteExact(const std::uint8_t* buffer, std::size_t size,
                                 Duration timeout) {
  return WritePipeExact(pipe_, buffer, size, timeout);
}

bool NamedPipeClient::Send(const Frame& frame) {
  const auto bytes = EncodeFrame(frame);
  if (bytes.empty()) {
    broken_ = true;
    return false;
  }
  if (!WriteExact(bytes.data(), bytes.size(), Duration{3000})) {
    broken_ = true;
    return false;
  }
  return true;
}

std::optional<Frame> NamedPipeClient::Receive(Duration timeout) {
  if (pipe_ == nullptr) {
    broken_ = true;
    return std::nullopt;
  }
  Frame frame;
  const auto status = ReceiveFrame(pipe_, timeout, &frame);
  if (status == ReadStatus::Frame) return frame;
  if (status == ReadStatus::Error) broken_ = true;
  return std::nullopt;
}

} // namespace htmlppt

#endif // _WIN32
