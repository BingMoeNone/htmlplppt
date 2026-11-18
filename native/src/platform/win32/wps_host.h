#pragma once

#ifdef _WIN32

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/deck_info.h"
#include "core/presentation_host.h"
#include "platform/win32/com_dispatch.h"

namespace htmlppt {

struct WpsHostConfig {
  int launch_attempts = 3;
  Duration launch_retry_delay{1000};
  int show_attempts = 3;
  Duration show_poll_interval{100};
  Duration show_start_timeout{2500};
  Duration show_settle_delay{150};
  int show_exit_attempts = 3;
  Duration show_exit_retry_delay{300};
  int jump_attempts = 3;
  Duration jump_retry_delay{300};
};

class WpsHost final : public PresentationHost,
                      public DeckInfoProvider {
 public:
  using AppFactory = std::function<DispatchPtr(long* error)>;

  explicit WpsHost(Clock& clock);
  WpsHost(Clock& clock, WpsHostConfig config);
  WpsHost(Clock& clock, AppFactory factory, WpsHostConfig config = {});
  ~WpsHost() override;

  WpsHost(const WpsHost&) = delete;
  WpsHost& operator=(const WpsHost&) = delete;

  HostResult Launch(const std::filesystem::path& deck_path) override;
  HostResult StartShow() override;
  ShowState QueryShow() override;
  HostResult ExitShow() override;
  HostResult ResumeShow(int slide) override;
  HostResult FocusShow() override;
  void Release() override;

  DeckInfoResult Load(const std::filesystem::path& deck_path) override;

  // Exports one BMP per slide into `out_dir` through COM (Slide.Export at the
  // list's 96x72 thumbnail size) and fills `slides_out` with the UTF-8 paths, in
  // slide order. The deck is opened read-only for the scan and closed again on
  // every path, including a failure; a failed export reports the first reason in
  // `error` and leaves `slides_out` empty. This is a long COM call: the caller
  // runs it off the UI thread.
  bool ExportSlideThumbnails(const std::filesystem::path& deck,
                             const std::filesystem::path& out_dir,
                             std::vector<std::string>* slides_out,
                             std::string* error);

 private:
  HostResult EnsureApp();
  HostResult OpenDeck(const std::filesystem::path& deck_path, bool with_window);
  HostResult WaitForShowActive();
  HostResult JumpTo(int slide);
  DeckInfoResult LoadOpenDeckInfo(bool close_after);
  std::wstring QuerySlideTitle(IDispatch* slide);

  Clock& clock_;
  AppFactory factory_;
  WpsHostConfig config_;
  ComApartment apartment_;
  DispatchPtr app_;
  DispatchPtr presentation_;
  std::filesystem::path deck_path_;
  bool show_active_ = false;
};

} // namespace htmlppt

#endif // _WIN32
