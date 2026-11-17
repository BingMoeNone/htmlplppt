// probe-wps-events - empirical probe for WPS/PowerPoint slideshow COM events.
//
// It answers one question: does the automation server expose a connectable
// event interface whose SlideShowNextSlide fires when the show advances? If the
// answer is no (or the DISPID cannot be resolved), the watchdog keeps polling
// and the event sink is not worth implementing.
//
// Usage: htmlppt-probe-wps-events <deck.pptx>
// Prints the connection point IIDs, the resolved DISPID, and every event the
// sink receives while the probe advances three slides.

#define _WIN32_DCOM

#include <windows.h>
#include <objbase.h>
#include <ocidl.h>
#include <oleauto.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

class Dispatch {
 public:
  Dispatch() = default;
  explicit Dispatch(IDispatch* value) : value_(value) {}
  Dispatch(const Dispatch&) = delete;
  Dispatch& operator=(const Dispatch&) = delete;
  Dispatch(Dispatch&& other) noexcept : value_(other.value_) {
    other.value_ = nullptr;
  }
  Dispatch& operator=(Dispatch&& other) noexcept {
    if (this != &other) {
      Reset();
      value_ = other.value_;
      other.value_ = nullptr;
    }
    return *this;
  }
  ~Dispatch() { Reset(); }

  IDispatch* get() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }
  void Reset() {
    if (value_ != nullptr) value_->Release();
    value_ = nullptr;
  }

  Dispatch Get(const wchar_t* name) {
    if (value_ == nullptr) return {};
    DISPID id = 0;
    LPOLESTR raw = const_cast<LPOLESTR>(name);
    if (FAILED(value_->GetIDsOfNames(IID_NULL, &raw, 1, LOCALE_USER_DEFAULT,
                                     &id))) {
      return {};
    }
    DISPPARAMS params{};
    VARIANT result;
    VariantInit(&result);
    if (FAILED(value_->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                              DISPATCH_PROPERTYGET, &params, &result, nullptr,
                              nullptr))) {
      return {};
    }
    Dispatch out(result.pdispVal);
    if (result.vt == VT_DISPATCH && out) result.pdispVal->AddRef();
    VariantClear(&result);
    return out;
  }

  bool Put(const wchar_t* name, bool value_in) {
    if (value_ == nullptr) return false;
    DISPID id = 0;
    LPOLESTR raw = const_cast<LPOLESTR>(name);
    if (FAILED(value_->GetIDsOfNames(IID_NULL, &raw, 1, LOCALE_USER_DEFAULT,
                                     &id))) {
      return false;
    }
    VARIANT argument;
    VariantInit(&argument);
    argument.vt = VT_BOOL;
    argument.boolVal = value_in ? VARIANT_TRUE : VARIANT_FALSE;
    DISPPARAMS params{&argument, nullptr, 1, 0};
    const HRESULT hr = value_->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                                      DISPATCH_PROPERTYPUT, &params, nullptr,
                                      nullptr, nullptr);
    VariantClear(&argument);
    return SUCCEEDED(hr);
  }

  bool Call(const wchar_t* name, int int_argument, long* error) {
    if (value_ == nullptr) return false;
    DISPID id = 0;
    LPOLESTR raw = const_cast<LPOLESTR>(name);
    if (FAILED(value_->GetIDsOfNames(IID_NULL, &raw, 1, LOCALE_USER_DEFAULT,
                                     &id))) {
      return false;
    }
    VARIANT argument;
    VariantInit(&argument);
    argument.vt = VT_I4;
    argument.lVal = int_argument;
    DISPPARAMS params{&argument, nullptr, 1, 0};
    EXCEPINFO info{};
    const HRESULT hr = value_->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                                      DISPATCH_METHOD, &params, nullptr, &info,
                                      nullptr);
    if (error != nullptr) *error = info.scode;
    VariantClear(&argument);
    return SUCCEEDED(hr);
  }

  bool CallNoArgs(const wchar_t* name, long* error) {
    if (value_ == nullptr) return false;
    DISPID id = 0;
    LPOLESTR raw = const_cast<LPOLESTR>(name);
    if (FAILED(value_->GetIDsOfNames(IID_NULL, &raw, 1, LOCALE_USER_DEFAULT,
                                     &id))) {
      return false;
    }
    DISPPARAMS params{};
    EXCEPINFO info{};
    const HRESULT hr = value_->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                                      DISPATCH_METHOD, &params, nullptr, &info,
                                      nullptr);
    if (error != nullptr) *error = info.scode;
    return SUCCEEDED(hr);
  }

  Dispatch Item(int index) {
    if (value_ == nullptr) return {};
    DISPID id = 0;
    LPOLESTR name = const_cast<LPOLESTR>(L"Item");
    if (FAILED(value_->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT,
                                     &id))) {
      return {};
    }
    VARIANT argument;
    VariantInit(&argument);
    argument.vt = VT_I4;
    argument.lVal = index;
    DISPPARAMS params{&argument, nullptr, 1, 0};
    VARIANT result;
    VariantInit(&result);
    if (FAILED(value_->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                              DISPATCH_METHOD | DISPATCH_PROPERTYGET, &params,
                              &result, nullptr, nullptr))) {
      return {};
    }
    Dispatch out(result.pdispVal);
    if (result.vt == VT_DISPATCH && out) result.pdispVal->AddRef();
    VariantClear(&result);
    return out;
  }

 private:
  IDispatch* value_ = nullptr;
};

class Sink : public IDispatch {
 public:
  Sink() = default;
  virtual ~Sink() = default;

  HRESULT QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (iid == IID_IUnknown || iid == IID_IDispatch) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  ULONG Release() override {
    const LONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return static_cast<ULONG>(refs);
  }
  HRESULT GetTypeInfoCount(UINT* count) override {
    if (count == nullptr) return E_POINTER;
    *count = 0;
    return S_OK;
  }
  HRESULT GetTypeInfo(UINT, LCID, ITypeInfo** info) override {
    if (info != nullptr) *info = nullptr;
    return E_NOTIMPL;
  }
  HRESULT GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
    return E_NOTIMPL;
  }
  HRESULT Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS* params, VARIANT*,
                 EXCEPINFO*, UINT*) override {
    const UINT args = params == nullptr ? 0 : params->cArgs;
    std::wprintf(L"event: dispid=%ld argc=%u\n", static_cast<long>(id), args);
    if (args > 0 && params->rgvarg != nullptr) {
      VARIANT* first = &params->rgvarg[0];
      if (first->vt == VT_I4) {
        std::wprintf(L"  arg0 (VT_I4)=%ld\n", static_cast<long>(first->lVal));
      } else if (first->vt == VT_DISPATCH) {
        std::wprintf(L"  arg0 (VT_DISPATCH)\n");
      }
    }
    ++events_;
    return S_OK;
  }

  int events() const { return events_; }

 private:
  LONG refs_ = 1;
  int events_ = 0;
};

void Pump(int milliseconds) {
  const DWORD end = GetTickCount() + static_cast<DWORD>(milliseconds);
  MSG message;
  while (GetTickCount() < end) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(5);
  }
}

// Loads the automation type library registered for the class and resolves the
// event interface IID and DISPID for SlideShowNextSlide.
bool ResolveEvent(REFCLSID clsid, std::wstring* event_name, IID* event_iid,
                  DISPID* event_id) {
  wchar_t clsid_text[64] = {};
  if (StringFromGUID2(clsid, clsid_text, 64) == 0) return false;
  std::wstring key = L"CLSID\\";
  key += clsid_text;
  HKEY clsid_key = nullptr;
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ, &clsid_key) !=
      ERROR_SUCCESS) {
    return false;
  }
  wchar_t typelib_text[64] = {};
  DWORD size = sizeof(typelib_text);
  DWORD type = 0;
  const LSTATUS status = RegQueryValueExW(clsid_key, L"TypeLib", nullptr, &type,
                                          reinterpret_cast<BYTE*>(typelib_text),
                                          &size);
  RegCloseKey(clsid_key);
  if (status != ERROR_SUCCESS) return false;

  CLSID typelib_id{};
  if (FAILED(CLSIDFromString(typelib_text, &typelib_id))) return false;

  ITypeLib* library = nullptr;
  if (FAILED(LoadRegTypeLib(typelib_id, 0xFFFF, 0xFFFF, LOCALE_USER_DEFAULT,
                            &library)) &&
      FAILED(LoadRegTypeLib(typelib_id, 1, 0, LOCALE_USER_DEFAULT, &library))) {
    return false;
  }
  struct Guard {
    ITypeLib* library;
    ~Guard() {
      if (library != nullptr) library->Release();
    }
  } guard{library};

  LPOLESTR wanted = const_cast<LPOLESTR>(L"SlideShowNextSlide");
  const UINT count = library->GetTypeInfoCount();
  for (UINT i = 0; i < count; ++i) {
    ITypeInfo* info = nullptr;
    if (FAILED(library->GetTypeInfo(i, &info)) || info == nullptr) continue;
    TYPEATTR* attributes = nullptr;
    if (SUCCEEDED(info->GetTypeAttr(&attributes)) && attributes != nullptr) {
      DISPID id = 0;
      const bool found = SUCCEEDED(info->GetIDsOfNames(&wanted, 1, &id));
      if (found) {
        BSTR name = nullptr;
        if (SUCCEEDED(library->GetDocumentation(
                i, &name, nullptr, nullptr, nullptr)) &&
            name != nullptr) {
          *event_name = std::wstring(name, SysStringLen(name));
          SysFreeString(name);
        }
        *event_iid = attributes->guid;
        *event_id = id;
        info->ReleaseTypeAttr(attributes);
        info->Release();
        return true;
      }
      info->ReleaseTypeAttr(attributes);
    }
    info->Release();
  }
  return false;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::wprintf(L"usage: htmlppt-probe-wps-events <deck.pptx>\n");
    return 2;
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::wprintf(L"CoInitializeEx failed\n");
    return 1;
  }

  CLSID clsid{};
  if (FAILED(CLSIDFromProgID(L"Kwpp.Application", &clsid))) {
    std::wprintf(L"Kwpp.Application is not registered\n");
    CoUninitialize();
    return 1;
  }

  std::wstring event_name;
  IID event_iid{};
  DISPID event_id = 0;
  const bool resolved = ResolveEvent(clsid, &event_name, &event_iid, &event_id);
  std::wprintf(L"event interface resolved: %s\n", resolved ? L"yes" : L"no");
  if (resolved) {
    wchar_t iid_text[64] = {};
    StringFromGUID2(event_iid, iid_text, 64);
    std::wprintf(L"  name=%ls iid=%ls dispid=%ld\n", event_name.c_str(),
                 iid_text, static_cast<long>(event_id));
  }

  IDispatch* raw = nullptr;
  if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER,
                              IID_IDispatch,
                              reinterpret_cast<void**>(&raw)))) {
    std::wprintf(L"CoCreateInstance failed\n");
    CoUninitialize();
    return 1;
  }
  Dispatch app(raw);

  IConnectionPointContainer* container = nullptr;
  if (FAILED(raw->QueryInterface(IID_IConnectionPointContainer,
                                 reinterpret_cast<void**>(&container)))) {
    std::wprintf(L"no IConnectionPointContainer\n");
    CoUninitialize();
    return 1;
  }
  IEnumConnectionPoints* points = nullptr;
  container->EnumConnectionPoints(&points);
  std::wprintf(L"connection points:\n");
  if (points != nullptr) {
    IConnectionPoint* point = nullptr;
    ULONG fetched = 0;
    while (points->Next(1, &point, &fetched) == S_OK && point != nullptr) {
      IID iid{};
      point->GetConnectionInterface(&iid);
      wchar_t iid_text[64] = {};
      StringFromGUID2(iid, iid_text, 64);
      std::wprintf(L"  %ls\n", iid_text);
      point->Release();
    }
    points->Release();
  }

  // Advise on every connection point: the type library may not expose the
  // event interface by name (WPS registers a partial one), but the sink accepts
  // any DISPID, so what matters is whether events arrive at all.
  Sink* sink = new Sink();
  struct Advised {
    IConnectionPoint* point;
    DWORD cookie;
  };
  std::vector<Advised> advised;
  {
    IEnumConnectionPoints* all = nullptr;
    container->EnumConnectionPoints(&all);
    if (all != nullptr) {
      IConnectionPoint* point = nullptr;
      ULONG fetched = 0;
      while (all->Next(1, &point, &fetched) == S_OK && point != nullptr) {
        DWORD cookie = 0;
        if (SUCCEEDED(point->Advise(sink, &cookie))) {
          advised.push_back(Advised{point, cookie});
          std::wprintf(L"advised: cookie=%lu\n",
                       static_cast<unsigned long>(cookie));
        } else {
          std::wprintf(L"advise failed\n");
          point->Release();
        }
      }
      all->Release();
    }
  }
  container->Release();

  Dispatch presentations = app.Get(L"Presentations");
  bool opened = false;
  if (presentations) {
    DISPID open_id = 0;
    LPOLESTR open_name = const_cast<LPOLESTR>(L"Open");
    if (SUCCEEDED(presentations.get()->GetIDsOfNames(
            IID_NULL, &open_name, 1, LOCALE_USER_DEFAULT, &open_id))) {
      VARIANT argument;
      VariantInit(&argument);
      argument.vt = VT_BSTR;
      argument.bstrVal = SysAllocString(argv[1]);
      DISPPARAMS params{&argument, nullptr, 1, 0};
      VARIANT result;
      VariantInit(&result);
      opened = SUCCEEDED(presentations.get()->Invoke(
          open_id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params,
          &result, nullptr, nullptr));
      VariantClear(&result);
      VariantClear(&argument);
    }
  }
  std::wprintf(L"open deck: %s\n", opened ? L"ok" : L"failed");
  if (opened) {
    Dispatch presentation = presentations.Item(1);
    Dispatch settings = presentation.Get(L"SlideShowSettings");
    long show_error = 0;
    const bool started = settings.CallNoArgs(L"Run", &show_error);
    std::wprintf(L"start show: %s (0x%08lx)\n", started ? L"ok" : L"failed",
                 static_cast<unsigned long>(show_error));
    if (started) {
      Pump(1500);
      Dispatch windows = app.Get(L"SlideShowWindows");
      Dispatch window = windows.Item(1);
      Dispatch view = window.Get(L"View");
      Pump(700);  // baseline: what fires with no slide change?
      for (int slide = 2; slide <= 4; ++slide) {
        std::wprintf(L"--- goto %d\n", slide);
        view.Call(L"GotoSlide", slide, nullptr);
        Pump(700);
      }
      std::wprintf(L"events received: %d\n", sink->events());
      view.Call(L"Exit", 0, nullptr);
    }
    presentation.Call(L"Close", 0, nullptr);
  }

  for (auto& entry : advised) {
    entry.point->Unadvise(entry.cookie);
    entry.point->Release();
  }
  sink->Release();
  CoUninitialize();
  return 0;
}
