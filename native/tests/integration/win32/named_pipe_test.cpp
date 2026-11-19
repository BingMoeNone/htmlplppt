#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "core/supervisor_protocol.h"
#include "platform/win32/named_pipe.h"

using htmlppt::Duration;
using htmlppt::Frame;
using htmlppt::FrameType;
using htmlppt::NamedPipeClient;
using htmlppt::NamedPipeServer;

namespace {

std::wstring UniquePipeName() {
  static std::atomic<std::uint32_t> counter{0};
  return L"\\\\.\\pipe\\htmlppt-test-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(counter++);
}

Frame MakeFrame(std::uint32_t sequence, const std::string& text) {
  Frame frame;
  frame.header.sequence = sequence;
  frame.header.type = FrameType::Log;
  frame.payload.assign(text.begin(), text.end());
  return frame;
}

} // namespace

HTMLPPT_TEST_CASE(win32_named_pipe_round_trip) {
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  std::atomic<bool> client_ok{false};
  std::thread client_thread([&] {
    NamedPipeClient client;
    client_ok = client.Connect(name, Duration{3000}) &&
                client.Send(MakeFrame(7, "hello"));
    client.Close();
  });
  auto channel = server.Accept(Duration{3000});
  client_thread.join();
  HTMLPPT_CHECK(channel != nullptr);
  HTMLPPT_CHECK(client_ok.load());
  const auto frame = channel->Receive(Duration{3000});
  HTMLPPT_CHECK(frame.has_value());
  HTMLPPT_CHECK(!channel->broken());
  HTMLPPT_CHECK_EQ(frame->header.sequence, std::uint32_t{7});
  HTMLPPT_CHECK_EQ(std::string(frame->payload.begin(), frame->payload.end()),
                   std::string{"hello"});
  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(win32_named_pipe_multiple_frames) {
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  std::atomic<bool> client_ok{false};
  std::thread client_thread([&] {
    NamedPipeClient client;
    client_ok = client.Connect(name, Duration{3000}) &&
                client.Send(MakeFrame(1, "one")) &&
                client.Send(MakeFrame(2, "two")) &&
                client.Send(MakeFrame(3, "three"));
    client.Close();
  });
  auto channel = server.Accept(Duration{3000});
  client_thread.join();
  HTMLPPT_CHECK(channel != nullptr);
  HTMLPPT_CHECK(client_ok.load());
  for (std::uint32_t i = 1; i <= 3; ++i) {
    const auto frame = channel->Receive(Duration{3000});
    HTMLPPT_CHECK(frame.has_value());
    HTMLPPT_CHECK(!channel->broken());
    HTMLPPT_CHECK_EQ(frame->header.sequence, i);
  }
  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(win32_named_pipe_disconnect_returns_empty) {
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  std::atomic<bool> client_ok{false};
  std::thread client_thread([&] {
    NamedPipeClient client;
    client_ok = client.Connect(name, Duration{3000});
    client.Close();
  });
  auto channel = server.Accept(Duration{3000});
  client_thread.join();
  HTMLPPT_CHECK(channel != nullptr);
  HTMLPPT_CHECK(client_ok.load());
  const auto frame = channel->Receive(Duration{300});
  HTMLPPT_CHECK(!frame.has_value());
  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(win32_named_pipe_connect_timeout) {
  NamedPipeClient client;
  HTMLPPT_CHECK(!client.Connect(L"\\\\.\\pipe\\htmlppt-missing-pipe",
                                Duration{100}));
}

HTMLPPT_TEST_CASE(win32_named_pipe_receive_timeout_is_safe) {
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  std::atomic<bool> close_client{false};
  std::atomic<bool> client_ok{false};
  std::thread client_thread([&] {
    NamedPipeClient client;
    client_ok = client.Connect(name, Duration{3000});
    while (!close_client.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client.Close();
  });
  auto channel = server.Accept(Duration{3000});
  const auto frame =
      channel ? channel->Receive(Duration{100}) : std::nullopt;
  close_client = true;
  client_thread.join();
  HTMLPPT_CHECK(channel != nullptr);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(!frame.has_value());
  HTMLPPT_CHECK(!channel->broken());
  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(win32_named_pipe_accept_failure_does_not_wait) {
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  NamedPipeClient client;
  HTMLPPT_CHECK(client.Connect(name, Duration{3000}));
  client.Close();
  const auto start = std::chrono::steady_clock::now();
  auto channel = server.Accept(Duration{2000});
  const auto elapsed = std::chrono::steady_clock::now() - start;
  HTMLPPT_CHECK(elapsed < std::chrono::milliseconds(1500));
  if (channel != nullptr) channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(win32_named_pipe_payload_frames_do_not_desync) {
  // A read that completes just as its wait times out must count its bytes, not
  // report a clean timeout: the payload byte of a heartbeat then stays in the
  // stream and the next frame is parsed from the middle of the previous one.
  // Polling with a short timeout while payload frames arrive quickly hits that
  // window; a desync shows up as broken() or a lost/garbled frame.
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  constexpr int kFrameCount = 200;
  std::atomic<bool> client_ok{false};
  std::thread client_thread([&] {
    NamedPipeClient client;
    client_ok = client.Connect(name, Duration{3000});
    if (!client_ok.load()) return;
    for (int i = 1; i <= kFrameCount; ++i) {
      if (!client.Send(MakeFrame(static_cast<std::uint32_t>(i), "x"))) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.Close();
  });

  auto channel = server.Accept(Duration{3000});
  if (channel == nullptr) {
    // Join before failing: a joinable thread destroyed during unwinding would
    // turn a failed Accept into std::terminate.
    client_thread.join();
    HTMLPPT_CHECK(channel != nullptr);
    return;
  }
  std::vector<std::uint32_t> sequences;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (sequences.size() < static_cast<std::size_t>(kFrameCount) &&
         std::chrono::steady_clock::now() < deadline) {
    const auto frame = channel->Receive(Duration{1});
    if (frame.has_value()) {
      sequences.push_back(frame->header.sequence);
      continue;
    }
    if (channel->broken()) break;
  }
  client_thread.join();
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(!channel->broken());
  HTMLPPT_CHECK_EQ(sequences.size(), static_cast<std::size_t>(kFrameCount));
  for (std::size_t i = 1; i < sequences.size(); ++i) {
    HTMLPPT_CHECK_EQ(sequences[i], sequences[i - 1] + 1);
  }
  channel->Close();
  server.Close();
}

#endif // _WIN32
