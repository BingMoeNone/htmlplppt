#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <filesystem>
#include <string>
#include <utility>

#include "core/clock.h"
#include "core/config_issue.h"
#include "fake_wps.h"
#include "platform/win32/wps_host.h"

using htmlppt::DispatchPtr;
using htmlppt::FakeClock;
using htmlppt::HasErrorIssue;
using htmlppt::HostStatus;
using htmlppt::WpsHost;
using htmlppt::WpsHostConfig;
using htmlppt::test::FakeWps;

namespace {

DispatchPtr FakeFactory(FakeWps* fake, long* error) {
  *error = 0;
  return fake->app;
}

} // namespace

HTMLPPT_TEST_CASE(win32_wps_host_loads_deck_info) {
  FakeClock clock;
  FakeWps fake;
  WpsHost host(clock,
               [&fake](long* error) { return FakeFactory(&fake, error); });
  const auto loaded =
      host.Load(std::filesystem::path(L"C:\\demo\\deck.pptx"));
  HTMLPPT_CHECK(!HasErrorIssue(loaded.issues));
  HTMLPPT_CHECK(loaded.info.has_value());
  HTMLPPT_CHECK_EQ(loaded.info->slides.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(loaded.info->slides[0].title, std::string("Title 1"));
  HTMLPPT_CHECK_EQ(loaded.info->slides[1].name, std::string("Slide2"));
  HTMLPPT_CHECK_EQ(loaded.info->slides[2].index, 3);
  HTMLPPT_CHECK_EQ(fake.close_count, 1);
  host.Release();
}

HTMLPPT_TEST_CASE(win32_wps_host_controls_show) {
  FakeClock clock;
  FakeWps fake;
  WpsHost host(clock,
               [&fake](long* error) { return FakeFactory(&fake, error); });
  HTMLPPT_CHECK(host.Launch(L"C:\\demo\\deck.pptx").ok());
  HTMLPPT_CHECK_EQ(fake.open_count, 1);

  HTMLPPT_CHECK(host.StartShow().ok());
  auto state = host.QueryShow();
  HTMLPPT_CHECK_EQ(static_cast<int>(state.status),
                   static_cast<int>(HostStatus::Ok));
  HTMLPPT_CHECK(state.active);
  HTMLPPT_CHECK_EQ(state.slide, 1);

  HTMLPPT_CHECK(host.ResumeShow(2).ok());
  state = host.QueryShow();
  HTMLPPT_CHECK(state.active);
  HTMLPPT_CHECK_EQ(state.slide, 2);

  HTMLPPT_CHECK(host.ExitShow().ok());
  state = host.QueryShow();
  HTMLPPT_CHECK(!state.active);
  HTMLPPT_CHECK_EQ(state.slide, 0);
  host.Release();
}

HTMLPPT_TEST_CASE(win32_wps_host_reports_busy) {
  FakeClock clock;
  FakeWps fake;
  WpsHost host(clock,
               [&fake](long* error) { return FakeFactory(&fake, error); });
  HTMLPPT_CHECK(host.Launch(L"C:\\demo\\deck.pptx").ok());
  HTMLPPT_CHECK(host.StartShow().ok());

  fake.position_override = [](VARIANT*, EXCEPINFO* exception) {
    if (exception != nullptr) exception->scode = 462;
    return static_cast<HRESULT>(DISP_E_EXCEPTION);
  };
  auto state = host.QueryShow();
  HTMLPPT_CHECK_EQ(static_cast<int>(state.status),
                   static_cast<int>(HostStatus::Busy));
  HTMLPPT_CHECK_EQ(state.error, 462L);

  fake.position_override = nullptr;
  state = host.QueryShow();
  HTMLPPT_CHECK_EQ(static_cast<int>(state.status),
                   static_cast<int>(HostStatus::Ok));
  HTMLPPT_CHECK(state.active);
  host.Release();
}

#endif // _WIN32
