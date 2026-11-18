#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/clock.h"
#include "core/config_issue.h"
#include "core/utf8_path.h"
#include "fake_wps.h"
#include "platform/win32/wps_host.h"
#include "test_utils.h"

using htmlppt::DispatchPtr;
using htmlppt::FakeClock;
using htmlppt::HasErrorIssue;
using htmlppt::HostStatus;
using htmlppt::IssueSeverity;
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

HTMLPPT_TEST_CASE(win32_wps_host_unavailable_warning) {
  FakeClock clock;
  WpsHostConfig config;
  config.launch_attempts = 1;
  WpsHost host(
      clock,
      [](long* error) {
        *error = static_cast<long>(REGDB_E_CLASSNOTREG);
        return DispatchPtr{};
      },
      config);
  const auto loaded =
      host.Load(std::filesystem::path(L"C:\\demo\\deck.pptx"));
  HTMLPPT_CHECK(!loaded.info.has_value());
  HTMLPPT_CHECK(!HasErrorIssue(loaded.issues));
  HTMLPPT_CHECK_EQ(loaded.issues.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(static_cast<int>(loaded.issues[0].severity),
                   static_cast<int>(IssueSeverity::Warning));
  HTMLPPT_CHECK(loaded.issues[0].message.find("--deck-info") !=
                std::string::npos);
  host.Release();
}

HTMLPPT_TEST_CASE(win32_wps_host_exports_slide_thumbnails) {
  FakeClock clock;
  FakeWps fake;
  htmlppt::test::TempDir dir;
  WpsHost host(clock,
               [&fake](long* error) { return FakeFactory(&fake, error); });
  std::vector<std::string> paths;
  std::string error = "stale";
  HTMLPPT_CHECK(host.ExportSlideThumbnails(
      std::filesystem::path(L"C:\\demo\\deck.pptx"), dir.path(), &paths,
      &error));
  HTMLPPT_CHECK(error.empty());
  // One export per slide, in slide order, at the list's fixed 96x72 size.
  HTMLPPT_CHECK_EQ(paths.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(fake.exported_files.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(fake.export_filter, std::string("BMP"));
  HTMLPPT_CHECK_EQ(fake.export_width, 96);
  HTMLPPT_CHECK_EQ(fake.export_height, 72);
  // The deck was opened read-only once and closed again.
  HTMLPPT_CHECK_EQ(fake.open_count, 1);
  HTMLPPT_CHECK_EQ(fake.close_count, 1);
  HTMLPPT_CHECK_EQ(htmlppt::PathFromUtf8(paths[0]).filename(),
                   std::filesystem::path(L"slide-01.bmp"));
  HTMLPPT_CHECK_EQ(htmlppt::PathFromUtf8(paths[2]).filename(),
                   std::filesystem::path(L"slide-03.bmp"));
  // The exported file name is the path the caller is handed back.
  HTMLPPT_CHECK_EQ(std::filesystem::path(fake.exported_files[0]),
                   htmlppt::PathFromUtf8(paths[0]));
  HTMLPPT_CHECK_EQ(htmlppt::PathFromUtf8(paths[0]).parent_path(), dir.path());
  host.Release();
}

HTMLPPT_TEST_CASE(win32_wps_host_closes_the_deck_when_an_export_fails) {
  FakeClock clock;
  FakeWps fake;
  htmlppt::test::TempDir dir;
  fake.export_fail_from = 2;
  WpsHost host(clock,
               [&fake](long* error) { return FakeFactory(&fake, error); });
  std::vector<std::string> paths;
  std::string error;
  HTMLPPT_CHECK(!host.ExportSlideThumbnails(
      std::filesystem::path(L"C:\\demo\\deck.pptx"), dir.path(), &paths,
      &error));
  HTMLPPT_CHECK(error.find("slide 2") != std::string::npos);
  HTMLPPT_CHECK(paths.empty());
  // A partial failure still closes the deck the host opened.
  HTMLPPT_CHECK_EQ(fake.close_count, 1);
  HTMLPPT_CHECK_EQ(fake.exported_files.size(), std::size_t{1});
  host.Release();
}

HTMLPPT_TEST_CASE(win32_wps_host_reports_an_unavailable_thumbnail_export) {
  FakeClock clock;
  htmlppt::test::TempDir dir;
  WpsHostConfig config;
  config.launch_attempts = 1;
  WpsHost host(
      clock,
      [](long* error) {
        *error = static_cast<long>(REGDB_E_CLASSNOTREG);
        return DispatchPtr{};
      },
      config);
  std::vector<std::string> paths;
  std::string error;
  HTMLPPT_CHECK(!host.ExportSlideThumbnails(
      std::filesystem::path(L"C:\\demo\\deck.pptx"), dir.path(), &paths,
      &error));
  HTMLPPT_CHECK(error.find("WPS") != std::string::npos);
  HTMLPPT_CHECK(paths.empty());
  host.Release();
}

#endif // _WIN32
