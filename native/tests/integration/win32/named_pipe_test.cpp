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

#endif // _WIN32
