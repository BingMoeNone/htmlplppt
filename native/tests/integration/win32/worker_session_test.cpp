#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "app/worker_session.h"
#include "core/supervisor.h"
#include "core/supervisor_protocol.h"
#include "fake_worker_driver.h"
#include "platform/win32/named_pipe.h"
#include "platform/win32/shared_blackbox.h"

using htmlppt::Duration;
using htmlppt::Frame;
using htmlppt::FrameType;
using htmlppt::NamedPipeClient;
using htmlppt::NamedPipeServer;
using htmlppt::RunWorkerSession;
using htmlppt::RunWithStartupHeartbeats;
using htmlppt::SharedBlackbox;
using htmlppt::WorkerSessionConfig;
using htmlppt::test::FakeWorkerDriver;

namespace {

std::wstring UniqueName(const wchar_t* prefix) {
  static std::atomic<std::uint32_t> counter{0};
  return std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(counter++);
}

std::wstring UniquePipeName() {
  return L"\\\\.\\pipe\\" + UniqueName(L"htmlppt-session-pipe-");
}

std::wstring UniqueBlackboxName() {
  return L"Local\\" + UniqueName(L"htmlppt-session-blackbox-");
}

Frame MakeStop(std::uint32_t sequence) {
  Frame stop;
  stop.header.type = FrameType::Stop;
  stop.header.sequence = sequence;
  stop.header.timestamp_ms = 1;
  return stop;
}

bool StrictlyIncreasing(const std::vector<std::uint32_t>& values) {
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (values[i] <= values[i - 1]) return false;
  }
  return true;
}

} // namespace

HTMLPPT_TEST_CASE(worker_session_heartbeats_and_stops) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));

  std::atomic<bool> client_ok{false};
  std::atomic<bool> saw_cleanup{false};
  std::vector<std::uint32_t> heartbeat_sequences;
  std::thread peer([&] {
    NamedPipeClient client;
    client_ok = client.Connect(pipe_name, Duration{3000});
    if (!client_ok.load()) return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto frame = client.Receive(Duration{50});
      if (!frame.has_value()) {
        if (client.broken()) return;
        continue;
      }
      if (frame->header.type == FrameType::Heartbeat) {
        heartbeat_sequences.push_back(frame->header.sequence);
        if (heartbeat_sequences.size() == 2) {
          if (!client.Send(MakeStop(1))) return;
        }
      } else if (frame->header.type == FrameType::Cleanup) {
        saw_cleanup = true;
        return;
      }
    }
  });

  auto channel = server.Accept(Duration{3000});
  HTMLPPT_CHECK(channel != nullptr);
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  FakeWorkerDriver driver;
  driver.step_sleep = Duration{1};
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(saw_cleanup.load());
  HTMLPPT_CHECK(heartbeat_sequences.size() >= std::size_t{2});
  HTMLPPT_CHECK(StrictlyIncreasing(heartbeat_sequences));
  HTMLPPT_CHECK_EQ(driver.start_calls, 1);
  HTMLPPT_CHECK_EQ(driver.request_stop_calls, 1);
  HTMLPPT_CHECK_EQ(driver.stop_reasons.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(driver.stop_reasons.front(), std::string{"supervisor stop"});
  HTMLPPT_CHECK(blackbox->appended() > 0);
  HTMLPPT_CHECK(error.empty());

  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(worker_session_survives_supervisor_loss) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));
  std::atomic<bool> client_ok{false};
  std::thread peer([&] {
    NamedPipeClient client;
    client_ok = client.Connect(pipe_name, Duration{3000});
    if (!client_ok.load()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    client.Close();
  });

  auto channel = server.Accept(Duration{3000});
  HTMLPPT_CHECK(channel != nullptr);
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  FakeWorkerDriver driver;
  driver.step_sleep = Duration{1};
  driver.steps_to_done = 400;
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  // Supervisor loss must not end the run or turn into a failure exit: the
  // driver is told once, keeps stepping, and the worker still exits 0.
  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK_EQ(driver.lost_calls, 1);
  HTMLPPT_CHECK_EQ(driver.lost_details.size(), std::size_t{1});
  HTMLPPT_CHECK(driver.step_calls_at_loss >= 0);
  HTMLPPT_CHECK(driver.step_calls > driver.step_calls_at_loss);
  HTMLPPT_CHECK(error.find("supervisor lost") != std::string::npos);
  HTMLPPT_CHECK_EQ(driver.request_stop_calls, 0);

  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(worker_session_heartbeats_during_slow_startup) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));
  std::atomic<bool> client_ok{false};
  std::vector<std::uint32_t> sequences;
  std::thread peer([&] {
    NamedPipeClient client;
    client_ok = client.Connect(pipe_name, Duration{3000});
    if (!client_ok.load()) return;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto frame = client.Receive(Duration{20});
      if (!frame.has_value()) {
        if (client.broken()) return;
        continue;
      }
      if (frame->header.type == FrameType::Heartbeat) {
        sequences.push_back(frame->header.sequence);
      }
    }
  });

  auto channel = server.Accept(Duration{3000});
  HTMLPPT_CHECK(channel != nullptr);
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  bool pipe_alive = false;
  const std::uint32_t last = RunWithStartupHeartbeats(
      *channel, *blackbox, config,
      [] { std::this_thread::sleep_for(std::chrono::milliseconds(60)); },
      &pipe_alive);
  peer.join();

  // A startup slower than the supervisor's heartbeat timeout must still see
  // heartbeats, and the session must continue from the last startup sequence.
  HTMLPPT_CHECK(pipe_alive);
  HTMLPPT_CHECK(last >= std::uint32_t{3});
  HTMLPPT_CHECK(sequences.size() >= std::size_t{3});
  HTMLPPT_CHECK_EQ(sequences.front(), std::uint32_t{1});
  HTMLPPT_CHECK_EQ(sequences.back(), last);
  HTMLPPT_CHECK(StrictlyIncreasing(sequences));
  HTMLPPT_CHECK(blackbox->appended() > 0);

  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(worker_session_heartbeats_during_blocking_step) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));
  std::atomic<bool> client_ok{false};
  std::atomic<bool> saw_cleanup{false};
  std::vector<std::uint32_t> sequences;
  std::thread peer([&] {
    NamedPipeClient client;
    client_ok = client.Connect(pipe_name, Duration{3000});
    if (!client_ok.load()) return;
    bool stop_sent = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto frame = client.Receive(Duration{20});
      if (!frame.has_value()) {
        if (client.broken()) return;
        continue;
      }
      if (frame->header.type == FrameType::Heartbeat) {
        sequences.push_back(frame->header.sequence);
        if (!stop_sent && sequences.size() >= 5) {
          stop_sent = true;
          if (!client.Send(MakeStop(1))) return;
        }
      } else if (frame->header.type == FrameType::Cleanup) {
        saw_cleanup = true;
        return;
      }
    }
  });

  auto channel = server.Accept(Duration{3000});
  if (channel == nullptr) {
    peer.join();
    HTMLPPT_CHECK(channel != nullptr);
    return;
  }
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  FakeWorkerDriver driver;
  // One Step blocks far longer than the heartbeat interval; the frames that
  // arrive meanwhile can only come from the session's heartbeat thread.
  driver.step_sleep = Duration{200};
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(saw_cleanup.load());
  HTMLPPT_CHECK_EQ(driver.step_calls, 1);
  HTMLPPT_CHECK(sequences.size() >= std::size_t{5});
  HTMLPPT_CHECK(StrictlyIncreasing(sequences));
  HTMLPPT_CHECK(error.empty());

  channel->Close();
  server.Close();
}

// I2: the helper thread heartbeats while Step() blocks, but loop progress may
// only advance after the step returns. The peer holds the loop in its first
// Step for 300 ms and records the progress carried by every heartbeat.
HTMLPPT_TEST_CASE(worker_session_progress_advances_after_blocking_step) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));
  std::atomic<bool> client_ok{false};
  std::atomic<bool> saw_cleanup{false};
  std::atomic<bool> payload_ok{true};
  std::atomic<bool> stop_sent{false};
  std::vector<std::uint64_t> progress;
  std::vector<std::chrono::steady_clock::time_point> times;
  std::thread peer([&] {
    NamedPipeClient client;
    client_ok = client.Connect(pipe_name, Duration{3000});
    if (!client_ok.load()) return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto frame = client.Receive(Duration{20});
      if (!frame.has_value()) {
        if (client.broken()) return;
        continue;
      }
      if (frame->header.type == FrameType::Heartbeat) {
        const auto decoded = htmlppt::DecodeHeartbeatPayload(frame->payload);
        if (!decoded.has_value()) {
          payload_ok = false;
          return;
        }
        progress.push_back(decoded->progress);
        times.push_back(std::chrono::steady_clock::now());
        if (!stop_sent && decoded->progress >= 1) {
          stop_sent = true;
          if (!client.Send(MakeStop(1))) return;
        }
      } else if (frame->header.type == FrameType::Cleanup) {
        saw_cleanup = true;
        return;
      }
    }
  });

  auto channel = server.Accept(Duration{3000});
  if (channel == nullptr) {
    peer.join();
    HTMLPPT_CHECK(channel != nullptr);
    return;
  }
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  FakeWorkerDriver driver;
  driver.first_step_sleep = Duration{300};
  driver.step_sleep = Duration{1};
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(saw_cleanup.load());
  HTMLPPT_CHECK(payload_ok.load());
  HTMLPPT_CHECK(stop_sent.load());
  HTMLPPT_CHECK(error.empty());
  HTMLPPT_CHECK(progress.size() >= std::size_t{5});

  std::size_t zeros = 0;
  std::size_t first_advance = progress.size();
  bool monotonic = true;
  for (std::size_t i = 0; i < progress.size(); ++i) {
    if (progress[i] == 0) ++zeros;
    if (i > 0 && progress[i] < progress[i - 1]) monotonic = false;
    if (progress[i] >= 1 && first_advance == progress.size()) {
      first_advance = i;
    }
  }
  // The blocked step spans several heartbeats, all with frozen progress...
  HTMLPPT_CHECK(zeros >= std::size_t{5});
  // ...and the first advance is only observed after that step returned.
  HTMLPPT_CHECK(first_advance < progress.size());
  HTMLPPT_CHECK(times[first_advance] - times.front() >=
                std::chrono::milliseconds(250));
  HTMLPPT_CHECK(monotonic);

  channel->Close();
  server.Close();
}

// A Step() blocked for ~3 s is a bounded COM action, not a hang: the heartbeat
// helper keeps the sequence advancing while progress stays frozen, and the
// default progress_timeout (12 s) must leave the worker running. A shadow
// SupervisorCore consumes the real session heartbeats so both thresholds (2 s
// sequence, 12 s progress) are exercised together.
HTMLPPT_TEST_CASE(worker_session_slow_step_is_not_reported_hung) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));

  std::atomic<bool> client_ok{false};
  std::atomic<bool> saw_cleanup{false};
  std::atomic<bool> stop_sent{false};
  std::atomic<bool> hung{false};
  std::atomic<bool> core_worker_running{true};
  std::size_t heartbeats = 0;
  long long first_heartbeat_ms = 0;
  long long first_advance_ms = 0;

  std::thread peer([&] {
    NamedPipeClient client;
    client_ok = client.Connect(pipe_name, Duration{3000});
    if (!client_ok.load()) return;
    htmlppt::SteadyClock clock;
    htmlppt::SupervisorCore core(htmlppt::SupervisorConfig{}, clock);
    core.Start();
    const auto note_hang =
        [&](const std::vector<htmlppt::SupervisorAction>& actions) {
          for (const auto& action : actions) {
            if (action.kind == htmlppt::SupervisorActionKind::RequestDump ||
                action.kind == htmlppt::SupervisorActionKind::TerminateWorker ||
                action.kind == htmlppt::SupervisorActionKind::RestartWorker) {
              hung = true;
            }
          }
        };
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto frame = client.Receive(Duration{20});
      if (!frame.has_value()) {
        if (client.broken()) return;
        continue;
      }
      if (frame->header.type == FrameType::Heartbeat) {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now()
                                    .time_since_epoch())
                                .count();
        ++heartbeats;
        if (first_heartbeat_ms == 0) first_heartbeat_ms = now_ms;
        htmlppt::WorkerEvent event;
        event.kind = htmlppt::WorkerEventKind::Heartbeat;
        event.at = clock.now();
        event.sequence = frame->header.sequence;
        const auto payload = htmlppt::DecodeHeartbeatPayload(frame->payload);
        if (payload.has_value()) {
          event.progress = payload->progress;
        }
        note_hang(core.Handle(event));
        note_hang(core.Tick());
        core_worker_running = core.worker_running();
        if (payload.has_value() && payload->progress >= 1) {
          if (first_advance_ms == 0) first_advance_ms = now_ms;
          if (!stop_sent) {
            stop_sent = true;
            if (!client.Send(MakeStop(1))) return;
          }
        }
      } else if (frame->header.type == FrameType::Cleanup) {
        saw_cleanup = true;
        core_worker_running = core.worker_running();
        return;
      }
    }
  });

  auto channel = server.Accept(Duration{3000});
  if (channel == nullptr) {
    peer.join();
    HTMLPPT_CHECK(channel != nullptr);
    return;
  }
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  FakeWorkerDriver driver;
  driver.first_step_sleep = Duration{3000};
  driver.step_sleep = Duration{1};
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(saw_cleanup.load());
  HTMLPPT_CHECK(stop_sent.load());
  // The shadow supervisor saw a live sequence with frozen progress for ~3 s
  // and must not have dumped, terminated, or restarted the worker.
  HTMLPPT_CHECK(!hung.load());
  HTMLPPT_CHECK(core_worker_running.load());
  HTMLPPT_CHECK(heartbeats >= std::size_t{20});
  HTMLPPT_CHECK(first_advance_ms - first_heartbeat_ms >= 2500);

  channel->Close();
  server.Close();
}

#endif // _WIN32

// Spec 11.3: the legacy stop-live.vbs deletes watcher.lock. The session must
// treat that as a stop request with its own reason, journalled once.
HTMLPPT_TEST_CASE(worker_session_external_stop_requests_a_stop) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));

  std::atomic<bool> client_ok{false};
  std::atomic<bool> saw_cleanup{false};
  std::thread peer([&] {
    NamedPipeClient client;
    client_ok = client.Connect(pipe_name, Duration{3000});
    if (!client_ok.load()) return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto frame = client.Receive(Duration{50});
      if (!frame.has_value()) {
        if (client.broken()) return;
        continue;
      }
      if (frame->header.type == FrameType::Cleanup) {
        saw_cleanup = true;
        return;
      }
    }
  });

  auto channel = server.Accept(Duration{3000});
  HTMLPPT_CHECK(channel != nullptr);
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  FakeWorkerDriver driver;
  driver.step_sleep = Duration{1};
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  config.external_stop = [] { return true; };
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(saw_cleanup.load());
  HTMLPPT_CHECK_EQ(driver.request_stop_calls, 1);
  HTMLPPT_CHECK_EQ(driver.stop_reasons.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(driver.stop_reasons.front(),
                   std::string{"legacy stop: watcher.lock removed"});
}