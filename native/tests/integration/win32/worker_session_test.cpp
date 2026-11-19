#include "test_main.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "test_utils.h"

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "app/worker_session.h"
#include "core/compat_flags.h"
#include "core/supervisor.h"
#include "core/supervisor_protocol.h"
#include "fake_worker_driver.h"
#include "platform/win32/named_pipe.h"
#include "platform/win32/shared_blackbox.h"

using htmlppt::CompatFlags;
using htmlppt::CompatIdentity;
using htmlppt::Duration;
using htmlppt::Frame;
using htmlppt::FrameType;
using htmlppt::LegacyLog;
using htmlppt::MakeCompatFlagMirror;
using htmlppt::NamedPipeClient;
using htmlppt::NamedPipeServer;
using htmlppt::RunWorkerSession;
using htmlppt::RunWithStartupHeartbeats;
using htmlppt::SharedBlackbox;
using htmlppt::State;
using htmlppt::StateSnapshot;
using htmlppt::WorkerSessionConfig;
using htmlppt::test::FakeWorkerDriver;
using htmlppt::test::TempDir;

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

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
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

// Hot reload may only run at a real safe point. The runtime owns that fact: the
// state byte can read Monitoring while the loop is inside Step, and the
// microsecond between two Steps is too narrow for a 200 ms heartbeat, so the
// driver sets a flag for its whole bounded Monitoring wait and the session copies
// it into every heartbeat. The fake driver models that wait. The `external_stop`
// hook parks the loop before the first Step, where no driver wait is running and
// the flag must stay clear; the Step parks below then advertise it.
HTMLPPT_TEST_CASE(worker_session_heartbeat_carries_the_driver_safe_point) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));

  struct Sample {
    std::uint64_t progress = 0;
    std::uint8_t state = 0;
    std::uint8_t flags = 0;
  };
  std::atomic<bool> client_ok{false};
  std::atomic<bool> saw_cleanup{false};
  std::atomic<bool> payload_ok{true};
  std::vector<Sample> samples;
  std::thread peer([&] {
    NamedPipeClient client;
    client_ok = client.Connect(pipe_name, Duration{3000});
    if (!client_ok.load()) return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
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
        samples.push_back(
            Sample{decoded->progress, decoded->state, decoded->flags});
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
  driver.state_snapshot.state = State::Monitoring;
  // Long enough that several 10 ms heartbeats fall inside each parked wait.
  driver.step_sleep = Duration{300};
  driver.steps_to_done = 2;
  std::atomic<int> stop_checks{0};
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  config.external_stop = [&stop_checks] {
    if (stop_checks.fetch_add(1) == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return false;
  };
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(saw_cleanup.load());
  HTMLPPT_CHECK(payload_ok.load());
  HTMLPPT_CHECK(error.empty());
  HTMLPPT_CHECK_EQ(driver.step_calls, 2);

  bool before_step = false;
  bool while_parked = false;
  for (const Sample& sample : samples) {
    // Every sample is a Monitoring heartbeat, so only the flag distinguishes
    // the two cases.
    HTMLPPT_CHECK_EQ(sample.state, static_cast<std::uint8_t>(State::Monitoring));
    if (sample.progress == 0 &&
        (sample.flags & htmlppt::kHeartbeatFlagSafePoint) == 0) {
      before_step = true;
    }
    if (sample.progress >= 1 &&
        (sample.flags & htmlppt::kHeartbeatFlagSafePoint) != 0) {
      while_parked = true;
    }
  }
  // Parked before the first Step: Monitoring state, no driver wait, no flag.
  HTMLPPT_CHECK(before_step);
  // Parked inside the second Step, which runs in Monitoring: the heartbeat
  // carries the driver's safe point.
  HTMLPPT_CHECK(while_parked);

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
HTMLPPT_TEST_CASE(worker_session_injected_supervisor_loss_does_not_stop_the_loop) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));

  std::atomic<bool> saw_cleanup{false};
  std::thread peer([&] {
    NamedPipeClient client;
    if (!client.Connect(pipe_name, Duration{3000})) return;
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
  config.inject_supervisor_loss = true;
  // The injected loss must not end the session: stop it through the driver's own
  // done() after a few steps.
  config.external_stop = [&driver] { return driver.step_calls >= 3; };
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK_EQ(driver.lost_calls, 1);
  HTMLPPT_CHECK_EQ(driver.lost_details.front(), std::string{"injected supervisor loss"});
  HTMLPPT_CHECK(saw_cleanup.load());
}

// A2: the session must publish every post-step snapshot to on_step. RunWorker
// builds the legacy flag mirror from exactly these calls, so the states and their
// order are part of the loop contract.
HTMLPPT_TEST_CASE(worker_session_on_step_sees_every_post_step_state) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));
  std::atomic<bool> client_ok{false};
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
      if (frame->header.type == FrameType::Cleanup) return;
    }
  });

  auto channel = server.Accept(Duration{3000});
  HTMLPPT_CHECK(channel != nullptr);
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  FakeWorkerDriver driver;
  driver.step_sleep = Duration{1};
  driver.scripted_states = {State::Monitoring, State::CoverIn, State::Live,
                            State::Monitoring};
  driver.steps_to_done = 4;
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  std::vector<State> observed;
  config.on_step = [&observed](const StateSnapshot& snapshot) {
    observed.push_back(snapshot.state);
  };
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(error.empty());
  const std::vector<State> expected{State::Monitoring, State::CoverIn,
                                    State::Live, State::Monitoring};
  HTMLPPT_CHECK_EQ(observed.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    HTMLPPT_CHECK_EQ(observed[index], expected[index]);
  }

  channel->Close();
  server.Close();
}

// A2: the callback RunWorker installs writes the mapped flags and one watch-log
// line per write. A full CoverIn -> Live -> Returning -> ReturnCoverOut ->
// Monitoring cycle must leave exactly the four protocol files and must not touch
// another run's files.
HTMLPPT_TEST_CASE(worker_session_mirrors_legacy_flags_for_a_scripted_live_cycle) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));
  std::atomic<bool> client_ok{false};
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
      if (frame->header.type == FrameType::Cleanup) return;
    }
  });

  auto channel = server.Accept(Duration{3000});
  HTMLPPT_CHECK(channel != nullptr);
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  TempDir dir;
  CompatFlags flags(dir.path(), CompatIdentity{"run-flags", 1234, ""});
  LegacyLog log(dir.path());
  // A different run owns the lock; the mirror must never delete or rewrite it.
  CompatFlags other(dir.path(), CompatIdentity{"other-run", 5678, ""});
  HTMLPPT_CHECK(other.WriteFlag(htmlppt::kLockFile, "other-lock"));

  FakeWorkerDriver driver;
  driver.step_sleep = Duration{1};
  driver.scripted_states = {State::CoverIn, State::Live, State::Returning,
                            State::ReturnCoverOut, State::Monitoring};
  driver.steps_to_done = 5;
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  config.on_step = MakeCompatFlagMirror(
      flags, log, [](const StateSnapshot&) {
        return std::string{"tef-live-page1"};
      });
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(error.empty());
  // focus-agent.ps1 reads the payload as "<profile>|<exitFlag>|<readyFlag>" and
  // writes those two paths itself, so the mirror must name them under the
  // compat directory instead of the legacy "exit=0|ready=1" pair.
  const std::string flag_dir = dir.path().string();
  const std::string agent_arm = "tef-live-page1|" + flag_dir +
                                "\\edge-closed.flag|" + flag_dir +
                                "\\cover-ready.flag";
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kCoverFadeFlag), std::string{});
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kCoverReadyFlag), std::string{});
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kAgentArmFlag), agent_arm);
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kEdgeClosedFlag), std::string{});
  for (const std::string_view name :
       {htmlppt::kCoverDoneFlag, htmlppt::kCoverStopFlag,
        htmlppt::kCoverHideFlag, htmlppt::kAgentStopFlag,
        htmlppt::kReturnRequestFlag}) {
    HTMLPPT_CHECK(!std::filesystem::exists(dir.path() / std::string(name)));
  }
  for (const std::string_view name :
       {htmlppt::kCoverFadeFlag, htmlppt::kCoverReadyFlag,
        htmlppt::kAgentArmFlag, htmlppt::kEdgeClosedFlag}) {
    HTMLPPT_CHECK(flags.IsOwned(name));
  }

  const std::string watch_log =
      ReadTextFile(dir.path() / std::string(htmlppt::kWatchLog));
  for (const std::string_view name :
       {htmlppt::kCoverFadeFlag, htmlppt::kCoverReadyFlag,
        htmlppt::kAgentArmFlag, htmlppt::kEdgeClosedFlag}) {
    HTMLPPT_CHECK(watch_log.find("flag " + std::string(name)) !=
                  std::string::npos);
  }
  HTMLPPT_CHECK_EQ(
      static_cast<std::size_t>(
          std::count(watch_log.begin(), watch_log.end(), '\n')),
      static_cast<std::size_t>(4));
  HTMLPPT_CHECK(watch_log.find(agent_arm) != std::string::npos);

  // Another run's flag is untouched: the mirror is ownership-aware.
  HTMLPPT_CHECK_EQ(other.ReadFlag(htmlppt::kLockFile),
                   std::string{"other-lock"});
  HTMLPPT_CHECK(other.IsOwned(htmlppt::kLockFile));

  channel->Close();
  server.Close();
}

// A2 fix round: a real stop collapses Stopping -> Exited inside one Step, so the
// loop observes ... -> Exited. That transition must still write the two stop
// flags the legacy stop tooling watches for.
HTMLPPT_TEST_CASE(worker_session_mirrors_stop_flags_for_a_scripted_stop_to_exited) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));
  std::atomic<bool> client_ok{false};
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
      if (frame->header.type == FrameType::Cleanup) return;
    }
  });

  auto channel = server.Accept(Duration{3000});
  HTMLPPT_CHECK(channel != nullptr);
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  TempDir dir;
  CompatFlags flags(dir.path(), CompatIdentity{"run-stop", 4321, ""});
  LegacyLog log(dir.path());

  FakeWorkerDriver driver;
  driver.step_sleep = Duration{1};
  driver.scripted_states = {State::CoverIn, State::Live, State::Exited};
  driver.steps_to_done = 3;
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  config.on_step = MakeCompatFlagMirror(
      flags, log, [](const StateSnapshot&) {
        return std::string{"tef-live-page1"};
      });
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(error.empty());
  const std::string flag_dir = dir.path().string();
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kCoverFadeFlag), std::string{});
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kCoverReadyFlag), std::string{});
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kAgentArmFlag),
                   "tef-live-page1|" + flag_dir + "\\edge-closed.flag|" +
                       flag_dir + "\\cover-ready.flag");
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kCoverStopFlag), std::string{});
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kAgentStopFlag), std::string{});
  // Leaving Live for Exited is a stop, not the return path: no edge-closed.
  HTMLPPT_CHECK(!std::filesystem::exists(
      dir.path() / std::string(htmlppt::kEdgeClosedFlag)));
  for (const std::string_view name :
       {htmlppt::kCoverFadeFlag, htmlppt::kCoverReadyFlag,
        htmlppt::kAgentArmFlag, htmlppt::kCoverStopFlag,
        htmlppt::kAgentStopFlag}) {
    HTMLPPT_CHECK(flags.IsOwned(name));
  }

  const std::string watch_log =
      ReadTextFile(dir.path() / std::string(htmlppt::kWatchLog));
  for (const std::string_view name :
       {htmlppt::kCoverFadeFlag, htmlppt::kCoverReadyFlag,
        htmlppt::kAgentArmFlag, htmlppt::kCoverStopFlag,
        htmlppt::kAgentStopFlag}) {
    HTMLPPT_CHECK(watch_log.find("flag " + std::string(name)) !=
                  std::string::npos);
  }
  HTMLPPT_CHECK_EQ(
      static_cast<std::size_t>(
          std::count(watch_log.begin(), watch_log.end(), '\n')),
      static_cast<std::size_t>(5));

  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(worker_session_pause_parks_the_loop_until_resume) {
  NamedPipeServer server;
  const std::wstring pipe_name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(pipe_name));

  std::atomic<bool> client_ok{false};
  std::atomic<bool> saw_paused{false};
  std::atomic<bool> resumed_progress{false};
  std::atomic<bool> progressed_while_paused{false};
  std::atomic<bool> stop_sent{false};
  std::thread peer([&] {
    using htmlppt::DecodeHeartbeatPayload;
    using htmlppt::kHeartbeatFlagPaused;
    NamedPipeClient client;
    client_ok = client.Connect(pipe_name, Duration{3000});
    if (!client_ok.load()) return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool pause_sent = false;
    bool resume_sent = false;
    bool hold_checked = false;
    std::uint64_t paused_progress = 0;
    Frame pause;
    pause.header.type = FrameType::Pause;
    pause.header.sequence = 1;
    Frame resume;
    resume.header.type = FrameType::Resume;
    resume.header.sequence = 2;
    const Frame stop = MakeStop(3);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto frame = client.Receive(Duration{10});
      if (!frame.has_value()) {
        if (client.broken()) return;
        continue;
      }
      if (frame->header.type == FrameType::Cleanup) return;
      if (frame->header.type != FrameType::Heartbeat) continue;
      const auto payload = DecodeHeartbeatPayload(frame->payload);
      if (!payload.has_value()) continue;
      const bool paused = (payload->flags & kHeartbeatFlagPaused) != 0;
      if (!pause_sent) {
        if (!client.Send(pause)) return;
        pause_sent = true;
      } else if (!saw_paused.load() && paused) {
        saw_paused = true;
        paused_progress = payload->progress;
        // Hold the pause for a while: any Step would advance progress.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        hold_checked = true;
      } else if (saw_paused.load() && paused && hold_checked &&
                 !resume_sent) {
        if (payload->progress != paused_progress) {
          progressed_while_paused = true;
        }
        if (!client.Send(resume)) return;
        resume_sent = true;
      } else if (saw_paused.load() && !paused &&
                 payload->progress > paused_progress) {
        // Resume applied: the loop stepped again.
        resumed_progress = true;
        if (!client.Send(stop)) return;
        stop_sent = true;
      }
      if (resumed_progress.load()) return;
    }
    if (!stop_sent.load()) client.Send(stop);
  });

  auto channel = server.Accept(Duration{3000});
  HTMLPPT_CHECK(channel != nullptr);
  auto blackbox = SharedBlackbox::Create(UniqueBlackboxName(), 64);
  HTMLPPT_CHECK(blackbox != nullptr);

  FakeWorkerDriver driver;
  driver.state_snapshot.state = State::Monitoring;
  driver.step_sleep = Duration{1};
  WorkerSessionConfig config;
  config.heartbeat_interval = Duration{10};
  std::string error;
  const int rc = RunWorkerSession(driver, *channel, *blackbox, config, &error);
  peer.join();

  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(saw_paused.load());
  HTMLPPT_CHECK(!progressed_while_paused.load());
  HTMLPPT_CHECK(resumed_progress.load());
  HTMLPPT_CHECK(driver.step_calls > 1);
  channel->Close();
  server.Close();
}

// Production writes the legacy flags off the loop thread. OnStep must not touch
// the filesystem on the caller's thread, and Flush must deliver every queued
// transition before the compat guard cleans up.
HTMLPPT_TEST_CASE(worker_session_async_flag_mirror_flushes_every_transition) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-async", GetCurrentProcessId(), ""});
  LegacyLog log(dir.path());
  htmlppt::CompatFlagMirror mirror(
      flags, log,
      [](const StateSnapshot&) { return std::string{"profile"}; },
      State::Monitoring);
  StateSnapshot live;
  live.state = State::Live;
  live.current_target_slide = 1;
  mirror.OnStep(live);
  mirror.Flush();

  HTMLPPT_CHECK(flags.IsOwned(htmlppt::kCoverReadyFlag));
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kCoverReadyFlag), std::string{});
  const std::string watch_log = ReadTextFile(dir.path() / std::string(htmlppt::kWatchLog));
  HTMLPPT_CHECK(watch_log.find("flag cover-ready.flag") != std::string::npos);
}