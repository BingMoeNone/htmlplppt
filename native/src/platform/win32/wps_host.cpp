#include "platform/win32/wps_host.h"

#include <utility>
#include <vector>

#include "core/config_issue.h"

namespace htmlppt {
namespace {

DispatchPtr GetObject(IDispatch* target, const wchar_t* name,
                      HostResult* call) {
  ComVariant value;
  *call = InvokeGet(target, name, &value);
  if (!call->ok()) return {};
  return VariantToDispatch(value.get(), call);
}

DispatchPtr GetItem(IDispatch* target, int index, HostResult* call) {
  std::vector<ComVariant> args;
  args.push_back(MakeInt(index));
  ComVariant value;
  *call = InvokeGetIndexed(target, L"Item", std::move(args), &value);
  if (!call->ok()) return {};
  return VariantToDispatch(value.get(), call);
}

HostResult Failure(HostStatus status, long error, std::string detail) {
  return {status, error, std::move(detail)};
}

bool GetIntProperty(IDispatch* target, const wchar_t* name, int* value,
                    HostResult* call) {
  ComVariant result;
  *call = InvokeGet(target, name, &result);
  if (!call->ok()) return false;
  if (!VariantToInt(result.get(), value)) {
    *call = Failure(HostStatus::Failed,
                    static_cast<long>(DISP_E_TYPEMISMATCH),
                    NarrowWide(name) + " is not an integer");
    return false;
  }
  return true;
}

} // namespace

WpsHost::WpsHost(Clock& clock) : WpsHost(clock, WpsHostConfig{}) {}

WpsHost::WpsHost(Clock& clock, WpsHostConfig config)
    : WpsHost(clock, AppFactory{}, config) {}

WpsHost::WpsHost(Clock& clock, AppFactory factory, WpsHostConfig config)
    : clock_(clock), factory_(std::move(factory)), config_(config) {}

WpsHost::~WpsHost() { Release(); }

HostResult WpsHost::EnsureApp() {
  if (app_) return {};
  if (!apartment_.ok()) {
    return Failure(HostStatus::Failed, apartment_.error(),
                   "COM apartment initialization failed: " +
                       HResultText(apartment_.error()));
  }
  long error = 0;
  for (int attempt = 0; attempt < config_.launch_attempts; ++attempt) {
    if (factory_) {
      app_ = factory_(&error);
    } else {
      app_ = DispatchPtr::FromProgId(L"Kwpp.Application", &error);
    }
    if (app_) break;
    if (attempt + 1 < config_.launch_attempts) {
      clock_.sleep_until(clock_.now() + config_.launch_retry_delay);
    }
  }
  if (!app_) {
    return Failure(ClassifyHresult(error), error,
                   "could not create Kwpp.Application: " +
                       HResultText(error));
  }
  (void)InvokePut(app_.get(), L"Visible", MakeBool(true));
  return {};
}

HostResult WpsHost::OpenDeck(const std::filesystem::path& deck_path,
                             bool with_window) {
  HostResult call;
  DispatchPtr presentations = GetObject(app_.get(), L"Presentations", &call);
  if (!presentations) return call;

  HostResult last = Failure(HostStatus::Failed, 0, "Presentations.Open failed");
  for (int attempt = 0; attempt < config_.launch_attempts; ++attempt) {
    std::vector<ComVariant> args;
    args.push_back(MakeString(deck_path.wstring()));
    args.push_back(MakeBool(true));   // ReadOnly
    args.push_back(MakeBool(false));  // Untitled
    args.push_back(MakeBool(with_window));
    ComVariant result;
    call = InvokeMethod(presentations.get(), L"Open", std::move(args), &result);
    if (call.ok()) {
      DispatchPtr opened = VariantToDispatch(result.get(), &call);
      if (!opened) return call;
      presentation_ = std::move(opened);
      deck_path_ = deck_path;
      return {};
    }
    last = call;
    if (call.status != HostStatus::Busy) break;
    if (attempt + 1 < config_.launch_attempts) {
      clock_.sleep_until(clock_.now() + config_.launch_retry_delay);
    }
  }
  return last;
}

HostResult WpsHost::Launch(const std::filesystem::path& deck_path) {
  HostResult result = EnsureApp();
  if (!result.ok()) return result;
  if (presentation_) {
    if (deck_path == deck_path_) return {};
    (void)InvokeMethod(presentation_.get(), L"Close", {}, nullptr);
    presentation_.Reset();
    deck_path_.clear();
  }
  return OpenDeck(deck_path, true);
}

HostResult WpsHost::WaitForShowActive() {
  const ClockPoint deadline = clock_.now() + config_.show_start_timeout;
  for (;;) {
    HostResult call;
    DispatchPtr windows = GetObject(app_.get(), L"SlideShowWindows", &call);
    if (!windows) return call;
    int count = 0;
    if (!GetIntProperty(windows.get(), L"Count", &count, &call)) return call;
    if (count > 0) {
      clock_.sleep_until(clock_.now() + config_.show_settle_delay);
      show_active_ = true;
      return {};
    }
    if (clock_.now() >= deadline) {
      return Failure(HostStatus::Failed, 0,
                     "slideshow did not become active");
    }
    clock_.sleep_until(clock_.now() + config_.show_poll_interval);
  }
}

HostResult WpsHost::StartShow() {
  if (!presentation_) {
    return Failure(HostStatus::Failed, 0, "deck is not open");
  }
  HostResult last =
      Failure(HostStatus::Failed, 0, "slideshow did not start");
  for (int attempt = 0; attempt < config_.show_attempts; ++attempt) {
    HostResult call;
    DispatchPtr settings =
        GetObject(presentation_.get(), L"SlideShowSettings", &call);
    if (!settings) {
      last = call;
    } else {
      (void)InvokePut(settings.get(), L"StartingSlide", MakeInt(1));
      (void)InvokePut(settings.get(), L"ShowWithAnimation", MakeBool(true));
      call = InvokeMethod(settings.get(), L"Run", {}, nullptr);
      if (call.ok()) {
        const HostResult wait = WaitForShowActive();
        if (wait.ok()) return {};
        last = wait;
      } else {
        last = call;
      }
    }
    if (attempt + 1 < config_.show_attempts) {
      clock_.sleep_until(clock_.now() + config_.show_exit_retry_delay);
    }
  }
  return last;
}

ShowState WpsHost::QueryShow() {
  ShowState state;
  if (!app_) {
    state.status = HostStatus::Unavailable;
    state.detail = "WPS is not running";
    return state;
  }
  HostResult call;
  DispatchPtr windows = GetObject(app_.get(), L"SlideShowWindows", &call);
  if (!windows) {
    state.status = call.status;
    state.error = call.error;
    state.detail = call.detail;
    return state;
  }
  int count = 0;
  if (!GetIntProperty(windows.get(), L"Count", &count, &call)) {
    state.status = call.status;
    state.error = call.error;
    state.detail = call.detail;
    return state;
  }
  if (count <= 0) {
    show_active_ = false;
    return state;
  }
  DispatchPtr window = GetItem(windows.get(), 1, &call);
  if (!window) {
    state.status = call.status;
    state.error = call.error;
    state.detail = call.detail;
    return state;
  }
  DispatchPtr view = GetObject(window.get(), L"View", &call);
  if (!view) {
    state.status = call.status;
    state.error = call.error;
    state.detail = call.detail;
    return state;
  }
  int position = 0;
  if (!GetIntProperty(view.get(), L"CurrentShowPosition", &position, &call)) {
    state.status = call.status;
    state.error = call.error;
    state.detail = call.detail;
    return state;
  }
  if (position < 1) {
    state.status = HostStatus::Failed;
    state.detail = "CurrentShowPosition is invalid";
    return state;
  }
  show_active_ = true;
  state.active = true;
  state.slide = position;
  return state;
}

HostResult WpsHost::ExitShow() {
  if (!app_) return {};
  HostResult last =
      Failure(HostStatus::Failed, 0, "slideshow did not exit");
  for (int attempt = 0; attempt < config_.show_exit_attempts; ++attempt) {
    HostResult call;
    DispatchPtr windows = GetObject(app_.get(), L"SlideShowWindows", &call);
    if (!windows) {
      last = call;
    } else {
      int count = 0;
      if (!GetIntProperty(windows.get(), L"Count", &count, &call)) {
        last = call;
      } else if (count <= 0) {
        show_active_ = false;
        return {};
      } else {
        DispatchPtr window = GetItem(windows.get(), 1, &call);
        DispatchPtr view;
        if (window) view = GetObject(window.get(), L"View", &call);
        if (!view) {
          last = call;
        } else {
          call = InvokeMethod(view.get(), L"Exit", {}, nullptr);
          if (!call.ok()) {
            last = call;
          } else {
            clock_.sleep_until(clock_.now() +
                               config_.show_exit_retry_delay);
            HostResult verify;
            DispatchPtr after =
                GetObject(app_.get(), L"SlideShowWindows", &verify);
            int remaining = 0;
            if (after &&
                GetIntProperty(after.get(), L"Count", &remaining, &verify) &&
                remaining <= 0) {
              show_active_ = false;
              return {};
            }
            last = verify.ok()
                       ? Failure(HostStatus::Failed, 0,
                                 "slideshow still active")
                       : verify;
          }
        }
      }
    }
    if (attempt + 1 < config_.show_exit_attempts) {
      clock_.sleep_until(clock_.now() + config_.show_exit_retry_delay);
    }
  }
  return last;
}

HostResult WpsHost::JumpTo(int slide) {
  if (slide < 1) {
    return Failure(HostStatus::Failed, 0, "slide must be >= 1");
  }
  HostResult last = Failure(HostStatus::Failed, 0, "goto slide failed");
  for (int attempt = 0; attempt < config_.jump_attempts; ++attempt) {
    HostResult call;
    DispatchPtr windows = GetObject(app_.get(), L"SlideShowWindows", &call);
    if (!windows) {
      last = call;
    } else {
      int count = 0;
      if (!GetIntProperty(windows.get(), L"Count", &count, &call)) {
        last = call;
      } else if (count <= 0) {
        last = Failure(HostStatus::Failed, 0, "no active slideshow");
      } else {
        DispatchPtr window = GetItem(windows.get(), 1, &call);
        DispatchPtr view;
        if (window) view = GetObject(window.get(), L"View", &call);
        if (!view) {
          last = call;
        } else {
          std::vector<ComVariant> args;
          args.push_back(MakeInt(slide));
          call = InvokeMethod(view.get(), L"GotoSlide", std::move(args),
                              nullptr);
          if (!call.ok()) {
            last = call;
          } else {
            clock_.sleep_until(clock_.now() + config_.jump_retry_delay);
            int current = 0;
            if (GetIntProperty(view.get(), L"CurrentShowPosition", &current,
                               &call) &&
                current == slide) {
              show_active_ = true;
              return {};
            }
            last = call.ok()
                       ? Failure(HostStatus::Failed, 0,
                                 "CurrentShowPosition did not change")
                       : call;
          }
        }
      }
    }
    if (attempt + 1 < config_.jump_attempts) {
      clock_.sleep_until(clock_.now() + config_.jump_retry_delay);
    }
  }
  return last;
}

HostResult WpsHost::ResumeShow(int slide) {
  const HostResult started = StartShow();
  if (!started.ok()) return started;
  (void)FocusShow();
  return JumpTo(slide);
}

HostResult WpsHost::FocusShow() {
  if (!app_) {
    return Failure(HostStatus::Unavailable, 0, "WPS is not running");
  }
  HostResult call;
  DispatchPtr windows = GetObject(app_.get(), L"SlideShowWindows", &call);
  if (!windows) return call;
  int count = 0;
  if (!GetIntProperty(windows.get(), L"Count", &count, &call)) return call;
  if (count <= 0) {
    return Failure(HostStatus::Failed, 0, "no active slideshow");
  }
  DispatchPtr window = GetItem(windows.get(), 1, &call);
  if (!window) return call;
  return InvokeMethod(window.get(), L"Activate", {}, nullptr);
}

void WpsHost::Release() {
  presentation_.Reset();
  app_.Reset();
  deck_path_.clear();
  show_active_ = false;
}

std::wstring WpsHost::QuerySlideTitle(IDispatch* slide) {
  HostResult call;
  DispatchPtr shapes = GetObject(slide, L"Shapes", &call);
  if (!shapes) return {};
  DispatchPtr title = GetObject(shapes.get(), L"Title", &call);
  if (!title) return {};
  DispatchPtr text_frame = GetObject(title.get(), L"TextFrame", &call);
  if (!text_frame) return {};
  DispatchPtr text_range = GetObject(text_frame.get(), L"TextRange", &call);
  if (!text_range) return {};
  ComVariant text;
  call = InvokeGet(text_range.get(), L"Text", &text);
  if (!call.ok()) return {};
  std::wstring wide;
  if (!VariantToWString(text.get(), &wide)) return {};
  return wide;
}

DeckInfoResult WpsHost::LoadOpenDeckInfo(bool close_after) {
  DeckInfoResult result;
  const auto close_deck = [&]() {
    if (!close_after) return;
    (void)InvokeMethod(presentation_.get(), L"Close", {}, nullptr);
    presentation_.Reset();
    deck_path_.clear();
  };

  HostResult call;
  DispatchPtr slides = GetObject(presentation_.get(), L"Slides", &call);
  if (!slides) {
    AddError(result.issues, "deck", "could not read Slides: " + call.detail);
    close_deck();
    return result;
  }
  int count = 0;
  if (!GetIntProperty(slides.get(), L"Count", &count, &call)) {
    AddError(result.issues, "deck",
             "could not read Slides.Count: " + call.detail);
    close_deck();
    return result;
  }
  if (count < 0) {
    AddError(result.issues, "deck", "Slides.Count is negative");
    close_deck();
    return result;
  }

  DeckInfo info;
  info.path = deck_path_;
  for (int index = 1; index <= count; ++index) {
    DispatchPtr slide = GetItem(slides.get(), index, &call);
    if (!slide) {
      AddError(result.issues, "deck.slide",
               "could not read slide " + std::to_string(index) + ": " +
                   call.detail);
      continue;
    }
    DeckSlideInfo item;
    item.index = index;
    ComVariant name_value;
    const HostResult name_call = InvokeGet(slide.get(), L"Name", &name_value);
    if (name_call.ok()) {
      std::wstring name;
      if (VariantToWString(name_value.get(), &name)) {
        item.name = NarrowWide(name);
      }
    }
    item.title = NarrowWide(QuerySlideTitle(slide.get()));
    info.slides.push_back(std::move(item));
  }
  result.info = std::move(info);
  close_deck();
  return result;
}

DeckInfoResult WpsHost::Load(const std::filesystem::path& deck_path) {
  DeckInfoResult result;
  const HostResult app_result = EnsureApp();
  if (!app_result.ok()) {
    AddWarning(result.issues, "deck",
               "WPS COM is unavailable; pass --deck-info to resolve without "
               "WPS (" +
                   app_result.detail + ")");
    return result;
  }

  bool close_after = false;
  if (!(presentation_ && deck_path_ == deck_path)) {
    if (presentation_) {
      (void)InvokeMethod(presentation_.get(), L"Close", {}, nullptr);
      presentation_.Reset();
      deck_path_.clear();
    }
    HostResult opened = OpenDeck(deck_path, false);
    if (!opened.ok() && opened.status == HostStatus::Failed) {
      opened = OpenDeck(deck_path, true);
    }
    if (!opened.ok()) {
      AddError(result.issues, "deck",
               "could not open the deck through WPS: " + opened.detail);
      return result;
    }
    close_after = true;
  }
  return LoadOpenDeckInfo(close_after);
}

} // namespace htmlppt
