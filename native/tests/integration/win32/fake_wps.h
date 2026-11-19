#pragma once

#ifdef _WIN32

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "fake_dispatch.h"
#include "platform/win32/com_dispatch.h"

namespace htmlppt::test {

// Fake Kwpp.Application tree with the subset of the Office object model the
// WpsHost uses: Presentations.Open, SlideShowSettings.Run, SlideShowWindows,
// SlideShowWindow.View, and Slides.Shapes.Title.TextFrame.TextRange.Text.
struct FakeWps {
  DispatchPtr app;
  DispatchPtr presentations;
  DispatchPtr presentation;
  DispatchPtr settings;
  DispatchPtr slides;
  std::vector<DispatchPtr> slide_objects;
  std::vector<DispatchPtr> shapes;
  std::vector<DispatchPtr> title_shapes;
  std::vector<DispatchPtr> text_frames;
  std::vector<DispatchPtr> text_ranges;
  DispatchPtr windows;
  DispatchPtr window;
  DispatchPtr view;
  int position = 1;
  int window_count = 0;
  int open_count = 0;
  int close_count = 0;
  std::function<HRESULT(VARIANT*, EXCEPINFO*)> position_override;
  // Slide::Export calls, in order: each slide's target file name (UTF-16) plus
  // the filter and scale arguments the host asked for. export_fail_from, when
  // set, makes that 1-based slide's export fail.
  std::vector<std::wstring> exported_files;
  std::string export_filter;
  int export_width = 0;
  int export_height = 0;
  int export_fail_from = 0;

  FakeWps() {
    app = MakeFake();
    presentations = MakeFake();
    presentation = MakeFake();
    settings = MakeFake();
    slides = MakeFake();
    windows = MakeFake();
    window = MakeFake();
    view = MakeFake();

    FakeDispatch* app_raw = Raw(app);
    FakeDispatch* presentations_raw = Raw(presentations);
    FakeDispatch* presentation_raw = Raw(presentation);
    FakeDispatch* settings_raw = Raw(settings);
    FakeDispatch* slides_raw = Raw(slides);
    FakeDispatch* windows_raw = Raw(windows);
    FakeDispatch* window_raw = Raw(window);
    FakeDispatch* view_raw = Raw(view);

    app_raw->SetObject(L"Presentations", presentations_raw);
    app_raw->SetObject(L"SlideShowWindows", windows_raw);
    app_raw->SetBool(L"Visible", false);

    presentations_raw->SetMethod(
        L"Open",
        [this](std::vector<VARIANT>, VARIANT* result, EXCEPINFO*) {
          ++open_count;
          SetDispatchResult(result, presentation);
          return S_OK;
        });

    presentation_raw->SetObject(L"SlideShowSettings", settings_raw);
    presentation_raw->SetObject(L"Slides", slides_raw);
    presentation_raw->SetMethod(
        L"Close",
        [this](std::vector<VARIANT>, VARIANT*, EXCEPINFO*) {
          ++close_count;
          return S_OK;
        });

    settings_raw->SetMethod(
        L"Run", [this](std::vector<VARIANT>, VARIANT*, EXCEPINFO*) {
          window_count = 1;
          position = 1;
          return S_OK;
        });

    slides_raw->SetInt(L"Count", 3);
    slides_raw->SetMethod(
        L"Item",
        [this](std::vector<VARIANT> args, VARIANT* result, EXCEPINFO*) {
          const int index = FirstInt(args);
          if (index < 1 || index > 3) {
            return static_cast<HRESULT>(DISP_E_BADINDEX);
          }
          SetDispatchResult(result,
                            slide_objects[static_cast<std::size_t>(index - 1)]);
          return S_OK;
        });

    windows_raw->SetGetter(
        L"Count", [this](VARIANT* result, EXCEPINFO*) {
          SetIntResult(result, window_count);
          return S_OK;
        });
    windows_raw->SetMethod(
        L"Item",
        [this](std::vector<VARIANT> args, VARIANT* result, EXCEPINFO*) {
          if (FirstInt(args) != 1) {
            return static_cast<HRESULT>(DISP_E_BADINDEX);
          }
          SetDispatchResult(result, window);
          return S_OK;
        });

    window_raw->SetObject(L"View", view_raw);
    window_raw->SetMethod(
        L"Activate", [](std::vector<VARIANT>, VARIANT*, EXCEPINFO*) {
          return S_OK;
        });

    view_raw->SetGetter(
        L"CurrentShowPosition",
        [this](VARIANT* result, EXCEPINFO* exception) {
          if (position_override) return position_override(result, exception);
          SetIntResult(result, position);
          return S_OK;
        });
    view_raw->SetMethod(
        L"Exit", [this](std::vector<VARIANT>, VARIANT*, EXCEPINFO*) {
          window_count = 0;
          return S_OK;
        });
    view_raw->SetMethod(
        L"GotoSlide",
        [this](std::vector<VARIANT> args, VARIANT*, EXCEPINFO*) {
          const int index = FirstInt(args);
          if (index < 1) return static_cast<HRESULT>(DISP_E_BADINDEX);
          position = index;
          return S_OK;
        });

    for (int index = 1; index <= 3; ++index) {
      DispatchPtr slide = MakeFake();
      DispatchPtr slide_shapes = MakeFake();
      DispatchPtr title = MakeFake();
      DispatchPtr text_frame = MakeFake();
      DispatchPtr text_range = MakeFake();

      FakeDispatch* slide_raw = Raw(slide);
      FakeDispatch* shapes_raw = Raw(slide_shapes);
      FakeDispatch* title_raw = Raw(title);
      FakeDispatch* text_frame_raw = Raw(text_frame);
      FakeDispatch* text_range_raw = Raw(text_range);

      slide_raw->SetObject(L"Shapes", shapes_raw);
      slide_raw->SetString(L"Name", L"Slide" + std::to_wstring(index));
      const int slide_number = index;
      slide_raw->SetMethod(
          L"Export",
          [this, slide_number](std::vector<VARIANT> args, VARIANT*,
                               EXCEPINFO*) {
            if (export_fail_from == slide_number) {
              return static_cast<HRESULT>(E_FAIL);
            }
            if (args.size() != 4 || args[0].vt != VT_BSTR ||
                args[1].vt != VT_BSTR || args[2].vt != VT_I4 ||
                args[3].vt != VT_I4) {
              return static_cast<HRESULT>(DISP_E_BADPARAMCOUNT);
            }
            exported_files.push_back(std::wstring(args[0].bstrVal));
            const std::wstring filter(args[1].bstrVal);
            export_filter.assign(filter.begin(), filter.end());
            export_width = args[2].lVal;
            export_height = args[3].lVal;
            return S_OK;
          });
      shapes_raw->SetObject(L"Title", title_raw);
      title_raw->SetObject(L"TextFrame", text_frame_raw);
      text_frame_raw->SetObject(L"TextRange", text_range_raw);
      text_range_raw->SetString(L"Text",
                                L"Title " + std::to_wstring(index));

      slide_objects.push_back(std::move(slide));
      shapes.push_back(std::move(slide_shapes));
      title_shapes.push_back(std::move(title));
      text_frames.push_back(std::move(text_frame));
      text_ranges.push_back(std::move(text_range));
    }
  }

  FakeWps(const FakeWps&) = delete;
  FakeWps& operator=(const FakeWps&) = delete;

  static DispatchPtr MakeFake() {
    return DispatchPtr(static_cast<IDispatch*>(new FakeDispatch()), false);
  }

  static FakeDispatch* Raw(const DispatchPtr& pointer) {
    return static_cast<FakeDispatch*>(pointer.get());
  }

  static int FirstInt(const std::vector<VARIANT>& args) {
    if (args.empty() || args[0].vt != VT_I4) return 0;
    return args[0].lVal;
  }

  static void SetIntResult(VARIANT* result, int value) {
    if (result == nullptr) return;
    VariantInit(result);
    result->vt = VT_I4;
    result->lVal = value;
  }

  static void SetDispatchResult(VARIANT* result, const DispatchPtr& object) {
    if (result == nullptr) return;
    VariantInit(result);
    result->vt = VT_DISPATCH;
    result->pdispVal = object.get();
    if (result->pdispVal != nullptr) result->pdispVal->AddRef();
  }
};

} // namespace htmlppt::test

#endif // _WIN32
