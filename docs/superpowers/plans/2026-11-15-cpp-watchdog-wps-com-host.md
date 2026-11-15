# C++ Watchdog WPS COM Host Implementation Plan (Plan 5A)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the Windows WPS COM host behind a platform-neutral `PresentationHost` interface, including late-bound `IDispatch` plumbing, COM error classification, deck-info loading through WPS, slideshow control, and automatic WPS deck info for `--validate` / `--resolve` when `--deck-info` is omitted.

**Architecture:** `core/presentation_host.h` defines the OS-neutral host contract (`HostResult`, `ShowState`, `PresentationHost`). `platform/win32/com_dispatch` owns COM apartment initialization, RAII dispatch pointers, variant helpers, named late-bound `IDispatch` calls, and HRESULT classification. `platform/win32/wps_host` maps the Office/WPS object model (`Kwpp.Application`, `Presentations.Open`, `SlideShowWindows`, `Slides`) onto the contract and also implements the existing `DeckInfoProvider` interface. Windows integration tests drive both layers with an in-process fake `IDispatch` tree, so CI needs no WPS installation. The CLI uses the WPS provider only when `--deck-info` is absent on Windows; Linux behavior is unchanged.

**Tech Stack:** C++20, Win32 COM (`ole32`, `oleaut32`), MSVC 19.51 via Visual Studio 18 Build Tools, the existing `Clock`/`DeckInfoProvider`/config-issue infrastructure, and the existing test harness.

## Global Constraints

- Target Windows 10/11 x64; the Linux/WSL diagnostic build must stay green.
- C++20; MSVC `/W4 /permissive- /utf-8 /WX`; GCC/Clang `-Wall -Wextra -Wpedantic -Werror`.
- No third-party runtime dependencies beyond the vendored nlohmann/json.
- All new code under `native/`; do not modify existing scripts, root tests, or production docs.
- Project-authored source must remain ASCII-only.
- `core/` stays platform-neutral. The `PresentationHost` interface is platform-neutral and lives in `core/`; every Windows API and COM call lives in `src/platform/win32/` under `_WIN32`.
- COM is late-bound (`IDispatch` + `GetIDsOfNames`/`Invoke`). Do not use `#import`, ATL, WIL, or generated type libraries.
- COM is initialized as a single-threaded apartment on the calling thread by `ComApartment`.
- `HostStatus::Busy` means a transient COM failure (including WPS error 462). `HostStatus::Unavailable` means the COM server is missing or disconnected. `HostStatus::Failed` is structural.
- `WpsHost::Release` releases COM references only. It never calls `Presentation.Close` or `Application.Quit` on a deck the watchdog did not open for a show, and it never terminates WPS.
- Existing Linux gate: 191/191 unit cases, CTest 8/8, `npm test` 3/3 with `CHROME_BIN`.
- Windows gate after this plan: `htmlppt_core_tests` 189/189, `htmlppt_win32_tests` 27/27 (22 existing + 5 new), CTest 9/9, `/WX` clean.

## Windows Build Toolchain

```text
CMake/CTest: C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\
Generator: Visual Studio 18 2026
MSVC: 19.51.36252.0 (14.51.36231)
Windows SDK: 10.0.26100.0
```

Use space-free symlinks if the sandbox rejects the quoted tool paths:

```bash
mkdir -p /tmp/wintools
ln -sf "/mnt/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" /tmp/wintools/cmake
ln -sf "/mnt/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe" /tmp/wintools/ctest
```

Windows build/test loop (from the worktree root):

```bash
WIN_SRC=/mnt/c/Temp/htmlplppt-win
rm -rf "$WIN_SRC" && mkdir -p "$WIN_SRC"
cp -r native "$WIN_SRC/native"
/tmp/wintools/cmake -S C:/Temp/htmlplppt-win/native -B C:/Temp/htmlplppt-win/build \
  -G "Visual Studio 18 2026" -A x64 -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
/tmp/wintools/cmake --build C:/Temp/htmlplppt-win/build --config Release --parallel
/tmp/wintools/ctest --test-dir C:/Temp/htmlplppt-win/build -C Release --output-on-failure
```

## File Structure

Create:

- `native/src/core/presentation_host.h` - platform-neutral presentation host contract.
- `native/src/platform/win32/com_dispatch.h` - COM apartment, dispatch pointer, variant helpers, late-bound invoke API, HRESULT classification.
- `native/src/platform/win32/com_dispatch.cpp` - COM helper implementation.
- `native/src/platform/win32/wps_host.h` - WPS presentation host and deck-info provider.
- `native/src/platform/win32/wps_host.cpp` - WPS object-model mapping and retry behavior.
- `native/tests/integration/win32/fake_dispatch.h` - in-process fake `IDispatch`.
- `native/tests/integration/win32/fake_wps.h` - fake `Kwpp.Application` object tree.
- `native/tests/integration/win32/com_dispatch_test.cpp` - COM helper tests.
- `native/tests/integration/win32/wps_host_test.cpp` - WPS host tests with the fake tree.

Modify:

- `native/CMakeLists.txt` - link `ole32` and `oleaut32` into `htmlppt_win32`.
- `native/src/app/cli.cpp` - use `WpsHost` for deck info on Windows when `--deck-info` is absent.
- `native/README.md` - document the WPS COM host and manual acceptance.
- `docs/superpowers/plans/2026-11-15-cpp-watchdog-wps-com-host.md` - as-built amendments.

---

### Task 1: Platform-Neutral Host Contract and COM Dispatch Plumbing

**Files:**
- Create: `native/src/core/presentation_host.h`
- Create: `native/src/platform/win32/com_dispatch.h`
- Create: `native/src/platform/win32/com_dispatch.cpp`
- Create: `native/tests/integration/win32/fake_dispatch.h`
- Create: `native/tests/integration/win32/com_dispatch_test.cpp`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Produces: `HostStatus`, `HostResult`, `ShowState`, `PresentationHost` in `core/presentation_host.h`.
- Produces: `ComApartment`, `DispatchPtr`, `ComVariant`, `MakeBool`, `MakeInt`, `MakeString`, `InvokeGet`, `InvokePut`, `InvokeMethod`, `InvokeGetIndexed`, `VariantToInt`, `VariantToBool`, `VariantToWString`, `VariantToDispatch`, `ClassifyHresult`, `HResultText`, `NarrowWide` in `platform/win32/com_dispatch.h`.
- Consumes: nothing beyond the C++ standard library and Win32 COM.

- [ ] **Step 1: Write the failing test**

Create `native/tests/integration/win32/com_dispatch_test.cpp`:

```cpp
#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include "fake_dispatch.h"
#include "platform/win32/com_dispatch.h"

using htmlppt::ComVariant;
using htmlppt::DispatchPtr;
using htmlppt::HostStatus;
using htmlppt::InvokeGet;
using htmlppt::InvokeMethod;
using htmlppt::InvokePut;
using htmlppt::MakeInt;
using htmlppt::VariantToInt;
using htmlppt::VariantToWString;
using htmlppt::test::FakeDispatch;

HTMLPPT_TEST_CASE(win32_com_dispatch_round_trip) {
  FakeDispatch object;
  object.SetString(L"Name", L"wps");
  object.SetInt(L"Count", 3);
  object.SetMethod(
      L"Add", [](std::vector<VARIANT> args, VARIANT* result, EXCEPINFO*) {
        if (args.size() != 2 || args[0].vt != VT_I4 || args[1].vt != VT_I4) {
          return static_cast<HRESULT>(DISP_E_TYPEMISMATCH);
        }
        if (result != nullptr) {
          VariantInit(result);
          result->vt = VT_I4;
          result->lVal = args[0].lVal + args[1].lVal;
        }
        return S_OK;
      });
  DispatchPtr target(&object);

  ComVariant name;
  const auto name_call = InvokeGet(target.get(), L"Name", &name);
  HTMLPPT_CHECK(name_call.ok());
  std::wstring text;
  HTMLPPT_CHECK(VariantToWString(name.get(), &text));
  HTMLPPT_CHECK_EQ(text, std::wstring(L"wps"));

  ComVariant count;
  HTMLPPT_CHECK(InvokeGet(target.get(), L"Count", &count).ok());
  int number = 0;
  HTMLPPT_CHECK(VariantToInt(count.get(), &number));
  HTMLPPT_CHECK_EQ(number, 3);

  std::vector<ComVariant> args;
  args.push_back(MakeInt(20));
  args.push_back(MakeInt(22));
  ComVariant sum;
  HTMLPPT_CHECK(
      InvokeMethod(target.get(), L"Add", std::move(args), &sum).ok());
  HTMLPPT_CHECK(VariantToInt(sum.get(), &number));
  HTMLPPT_CHECK_EQ(number, 42);

  HTMLPPT_CHECK(InvokePut(target.get(), L"Count", MakeInt(7)).ok());
  ComVariant updated;
  HTMLPPT_CHECK(InvokeGet(target.get(), L"Count", &updated).ok());
  HTMLPPT_CHECK(VariantToInt(updated.get(), &number));
  HTMLPPT_CHECK_EQ(number, 7);
}

HTMLPPT_TEST_CASE(win32_com_dispatch_classifies_busy) {
  FakeDispatch object;
  object.SetMethod(
      L"Boom", [](std::vector<VARIANT>, VARIANT*, EXCEPINFO* exception) {
        if (exception != nullptr) exception->scode = 462;
        return static_cast<HRESULT>(DISP_E_EXCEPTION);
      });
  DispatchPtr target(&object);

  ComVariant result;
  const auto call = InvokeMethod(target.get(), L"Boom", {}, &result);
  HTMLPPT_CHECK_EQ(static_cast<int>(call.status),
                   static_cast<int>(HostStatus::Busy));
  HTMLPPT_CHECK_EQ(call.error, 462L);
  HTMLPPT_CHECK(call.detail.find("0x000001CE") != std::string::npos);

  const auto missing = InvokeGet(target.get(), L"Missing", nullptr);
  HTMLPPT_CHECK_EQ(static_cast<int>(missing.status),
                   static_cast<int>(HostStatus::Failed));
}

#endif // _WIN32
```

- [ ] **Step 2: Run test to verify it fails**

Run the Windows loop. Expected: configure/build fails because
`core/presentation_host.h`, `platform/win32/com_dispatch.h`, and
`fake_dispatch.h` do not exist.

- [ ] **Step 3: Add the platform-neutral host contract**

Create `native/src/core/presentation_host.h`:

```cpp
#pragma once

#include <filesystem>
#include <string>

namespace htmlppt {

enum class HostStatus {
  Ok,
  Busy,
  Unavailable,
  Failed,
};

struct HostResult {
  HostStatus status = HostStatus::Ok;
  long error = 0;
  std::string detail;

  bool ok() const { return status == HostStatus::Ok; }
};

struct ShowState {
  HostStatus status = HostStatus::Ok;
  bool active = false;
  int slide = 0;
  long error = 0;
  std::string detail;
};

class PresentationHost {
 public:
  virtual ~PresentationHost() = default;

  // Create the presentation application, make it visible, and open the deck
  // read-only. Repeated calls with the same deck are a no-op.
  virtual HostResult Launch(const std::filesystem::path& deck_path) = 0;

  // Start the slideshow from slide 1 and wait until it is active.
  virtual HostResult StartShow() = 0;

  // Poll the slideshow window count and current position.
  virtual ShowState QueryShow() = 0;

  // Leave the slideshow while keeping the deck open.
  virtual HostResult ExitShow() = 0;

  // Ensure the show is running, focus it, and jump to the slide.
  virtual HostResult ResumeShow(int slide) = 0;

  // Focus the slideshow window without changing its position.
  virtual HostResult FocusShow() = 0;

  // Release COM references. Never closes the deck or quits the application.
  virtual void Release() = 0;
};

} // namespace htmlppt
```

- [ ] **Step 4: Add the COM dispatch header**

Create `native/src/platform/win32/com_dispatch.h`:

```cpp
#pragma once

#ifdef _WIN32

#include <windows.h>
#include <oaidl.h>
#include <oleauto.h>

#include <string>
#include <utility>
#include <vector>

#include "core/presentation_host.h"

namespace htmlppt {

class ComApartment {
 public:
  ComApartment();
  ~ComApartment();
  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  bool ok() const { return initialized_; }
  long error() const { return error_; }

 private:
  bool initialized_ = false;
  long error_ = 0;
};

class DispatchPtr {
 public:
  DispatchPtr() = default;
  explicit DispatchPtr(IDispatch* value, bool add_ref = true);
  ~DispatchPtr();
  DispatchPtr(const DispatchPtr& other);
  DispatchPtr& operator=(const DispatchPtr& other);
  DispatchPtr(DispatchPtr&& other) noexcept;
  DispatchPtr& operator=(DispatchPtr&& other) noexcept;

  IDispatch* get() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }
  void Reset();

  static DispatchPtr FromProgId(const wchar_t* prog_id, long* error);

 private:
  IDispatch* value_ = nullptr;
};

class ComVariant {
 public:
  ComVariant();
  ~ComVariant();
  ComVariant(const ComVariant&) = delete;
  ComVariant& operator=(const ComVariant&) = delete;
  ComVariant(ComVariant&& other) noexcept;
  ComVariant& operator=(ComVariant&& other) noexcept;

  VARIANT* out() { return &value_; }
  const VARIANT& get() const { return value_; }
  VARIANT& get() { return value_; }

 private:
  VARIANT value_;
};

ComVariant MakeBool(bool value);
ComVariant MakeInt(int value);
ComVariant MakeString(const std::wstring& value);

HostResult InvokeGet(IDispatch* target, const wchar_t* name,
                     ComVariant* result = nullptr);
HostResult InvokePut(IDispatch* target, const wchar_t* name, ComVariant value);
HostResult InvokeMethod(IDispatch* target, const wchar_t* name,
                        std::vector<ComVariant> args,
                        ComVariant* result = nullptr);
HostResult InvokeGetIndexed(IDispatch* target, const wchar_t* name,
                            std::vector<ComVariant> args,
                            ComVariant* result = nullptr);

bool VariantToInt(const VARIANT& value, int* out);
bool VariantToBool(const VARIANT& value, bool* out);
bool VariantToWString(const VARIANT& value, std::wstring* out);
DispatchPtr VariantToDispatch(const VARIANT& value, HostResult* call);

HostStatus ClassifyHresult(long error);
std::string HResultText(long error);
std::string NarrowWide(const std::wstring& text);

} // namespace htmlppt

#endif // _WIN32
```

- [ ] **Step 5: Add the COM dispatch implementation**

Create `native/src/platform/win32/com_dispatch.cpp`:

```cpp
#include "platform/win32/com_dispatch.h"

#include <cstdint>
#include <cstdio>
#include <utility>

namespace htmlppt {
namespace {

constexpr unsigned long kRpcCallRejected = 0x80010001UL;
constexpr unsigned long kRpcServerCallRetryLater = 0x8001010AUL;
constexpr unsigned long kDispatchException = 0x80020009UL;
constexpr unsigned long kClassNotRegistered = 0x80040154UL;
constexpr unsigned long kClassString = 0x800401F3UL;
constexpr unsigned long kMkUnavailable = 0x800401E3UL;
constexpr unsigned long kObjNotConnected = 0x800401FDUL;
constexpr unsigned long kRpcDisconnected = 0x80010108UL;
constexpr unsigned long kRpcServerDied = 0x80010007UL;
constexpr unsigned long kRpcServerDiedDne = 0x80010008UL;
constexpr unsigned long kServerExecFailure = 0x80080005UL;

std::string Narrow(const wchar_t* text) {
  return text == nullptr ? std::string() : NarrowWide(text);
}

HostResult InvokeInternal(IDispatch* target, const wchar_t* name, WORD flags,
                          std::vector<ComVariant> args,
                          ComVariant* result) {
  if (target == nullptr) {
    return {HostStatus::Failed, static_cast<long>(E_POINTER),
            "null IDispatch for " + Narrow(name)};
  }

  wchar_t* member_name = const_cast<wchar_t*>(name);
  DISPID id = 0;
  HRESULT hr = target->GetIDsOfNames(IID_NULL, &member_name, 1,
                                     LOCALE_USER_DEFAULT, &id);
  if (FAILED(hr)) {
    HostResult failure;
    failure.status = ClassifyHresult(static_cast<long>(hr));
    failure.error = static_cast<long>(hr);
    failure.detail = "GetIDsOfNames " + Narrow(name) +
                     " failed: " + HResultText(failure.error);
    return failure;
  }

  std::vector<VARIANT> reversed;
  reversed.reserve(args.size());
  for (auto it = args.rbegin(); it != args.rend(); ++it) {
    reversed.push_back(it->get());
  }

  DISPID put_id = DISPID_PROPERTYPUT;
  DISPPARAMS params{};
  params.cArgs = static_cast<UINT>(reversed.size());
  params.rgvarg = reversed.empty() ? nullptr : reversed.data();
  if ((flags & DISPATCH_PROPERTYPUT) != 0) {
    params.cNamedArgs = 1;
    params.rgdispidNamedArgs = &put_id;
  }

  EXCEPINFO exception{};
  UINT argument_error = 0;
  ComVariant local_result;
  VARIANT* result_ptr = result != nullptr ? local_result.out() : nullptr;
  hr = target->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &params,
                      result_ptr, &exception, &argument_error);

  HostResult call;
  if (FAILED(hr)) {
    call.error = static_cast<long>(hr);
    if (hr == static_cast<HRESULT>(kDispatchException) &&
        exception.scode != 0) {
      call.error = static_cast<long>(exception.scode);
    }
    call.status = ClassifyHresult(static_cast<long>(hr));
    call.detail = Narrow(name) + " failed: " + HResultText(call.error);
    if (hr == static_cast<HRESULT>(DISP_E_TYPEMISMATCH)) {
      call.detail += " (arg " + std::to_string(argument_error) + ")";
    }
    const std::string description = Narrow(exception.bstrDescription);
    if (!description.empty()) call.detail += ": " + description;
  } else {
    if (result != nullptr) *result = std::move(local_result);
    call.status = HostStatus::Ok;
  }

  if (exception.bstrDescription != nullptr) {
    SysFreeString(exception.bstrDescription);
  }
  if (exception.bstrSource != nullptr) SysFreeString(exception.bstrSource);
  if (exception.bstrHelpFile != nullptr) SysFreeString(exception.bstrHelpFile);
  return call;
}

} // namespace

ComApartment::ComApartment() {
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  error_ = static_cast<long>(hr);
  initialized_ = SUCCEEDED(hr);
}

ComApartment::~ComApartment() {
  if (initialized_) CoUninitialize();
}

DispatchPtr::DispatchPtr(IDispatch* value, bool add_ref) : value_(value) {
  if (value_ != nullptr && add_ref) value_->AddRef();
}

DispatchPtr::~DispatchPtr() { Reset(); }

DispatchPtr::DispatchPtr(const DispatchPtr& other) : value_(other.value_) {
  if (value_ != nullptr) value_->AddRef();
}

DispatchPtr& DispatchPtr::operator=(const DispatchPtr& other) {
  if (this != &other) {
    DispatchPtr copy(other);
    std::swap(value_, copy.value_);
  }
  return *this;
}

DispatchPtr::DispatchPtr(DispatchPtr&& other) noexcept : value_(other.value_) {
  other.value_ = nullptr;
}

DispatchPtr& DispatchPtr::operator=(DispatchPtr&& other) noexcept {
  if (this != &other) {
    Reset();
    value_ = other.value_;
    other.value_ = nullptr;
  }
  return *this;
}

void DispatchPtr::Reset() {
  if (value_ != nullptr) {
    value_->Release();
    value_ = nullptr;
  }
}

DispatchPtr DispatchPtr::FromProgId(const wchar_t* prog_id, long* error) {
  CLSID clsid{};
  HRESULT hr = CLSIDFromProgID(prog_id, &clsid);
  if (FAILED(hr)) {
    if (error != nullptr) *error = static_cast<long>(hr);
    return {};
  }
  IDispatch* dispatch = nullptr;
  hr = CoCreateInstance(clsid, nullptr, CLSCTX_SERVER, IID_IDispatch,
                        reinterpret_cast<void**>(&dispatch));
  if (FAILED(hr) || dispatch == nullptr) {
    if (error != nullptr) *error = static_cast<long>(hr);
    if (dispatch != nullptr) dispatch->Release();
    return {};
  }
  if (error != nullptr) *error = 0;
  return DispatchPtr(dispatch, false);
}

ComVariant::ComVariant() { VariantInit(&value_); }

ComVariant::~ComVariant() { VariantClear(&value_); }

ComVariant::ComVariant(ComVariant&& other) noexcept : value_(other.value_) {
  VariantInit(&other.value_);
}

ComVariant& ComVariant::operator=(ComVariant&& other) noexcept {
  if (this != &other) {
    VariantClear(&value_);
    value_ = other.value_;
    VariantInit(&other.value_);
  }
  return *this;
}

ComVariant MakeBool(bool value) {
  ComVariant variant;
  variant.out()->vt = VT_BOOL;
  variant.out()->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  return variant;
}

ComVariant MakeInt(int value) {
  ComVariant variant;
  variant.out()->vt = VT_I4;
  variant.out()->lVal = value;
  return variant;
}

ComVariant MakeString(const std::wstring& value) {
  ComVariant variant;
  variant.out()->vt = VT_BSTR;
  variant.out()->bstrVal =
      SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
  if (variant.out()->bstrVal == nullptr) variant.out()->vt = VT_EMPTY;
  return variant;
}

HostResult InvokeGet(IDispatch* target, const wchar_t* name,
                     ComVariant* result) {
  return InvokeInternal(target, name, DISPATCH_PROPERTYGET, {}, result);
}

HostResult InvokePut(IDispatch* target, const wchar_t* name, ComVariant value) {
  std::vector<ComVariant> args;
  args.push_back(std::move(value));
  return InvokeInternal(target, name, DISPATCH_PROPERTYPUT, std::move(args),
                        nullptr);
}

HostResult InvokeMethod(IDispatch* target, const wchar_t* name,
                        std::vector<ComVariant> args, ComVariant* result) {
  return InvokeInternal(target, name, DISPATCH_METHOD, std::move(args), result);
}

HostResult InvokeGetIndexed(IDispatch* target, const wchar_t* name,
                            std::vector<ComVariant> args,
                            ComVariant* result) {
  return InvokeInternal(target, name, DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                        std::move(args), result);
}

bool VariantToInt(const VARIANT& value, int* out) {
  if (out == nullptr) return false;
  switch (value.vt) {
    case VT_I4:
      *out = value.lVal;
      return true;
    case VT_I2:
      *out = value.iVal;
      return true;
    case VT_UI1:
      *out = value.bVal;
      return true;
    case VT_UI2:
      *out = value.uiVal;
      return true;
    case VT_UI4:
      *out = static_cast<int>(value.ulVal);
      return true;
    case VT_INT:
      *out = value.intVal;
      return true;
    case VT_UINT:
      *out = static_cast<int>(value.uintVal);
      return true;
    case VT_I8:
      *out = static_cast<int>(value.llVal);
      return true;
    case VT_BOOL:
      *out = value.boolVal != VARIANT_FALSE ? 1 : 0;
      return true;
    case VT_R8:
      *out = static_cast<int>(value.dblVal);
      return true;
    case VT_VARIANT | VT_BYREF:
      return value.pvarVal != nullptr && VariantToInt(*value.pvarVal, out);
    default:
      return false;
  }
}

bool VariantToBool(const VARIANT& value, bool* out) {
  if (out == nullptr) return false;
  if (value.vt == VT_BOOL) {
    *out = value.boolVal != VARIANT_FALSE;
    return true;
  }
  int number = 0;
  if (!VariantToInt(value, &number)) return false;
  *out = number != 0;
  return true;
}

bool VariantToWString(const VARIANT& value, std::wstring* out) {
  if (out == nullptr) return false;
  if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
    *out = std::wstring(value.bstrVal, SysStringLen(value.bstrVal));
    return true;
  }
  if ((value.vt & VT_BYREF) != 0 && value.vt == (VT_VARIANT | VT_BYREF) &&
      value.pvarVal != nullptr) {
    return VariantToWString(*value.pvarVal, out);
  }
  return false;
}

DispatchPtr VariantToDispatch(const VARIANT& value, HostResult* call) {
  if (value.vt != VT_DISPATCH || value.pdispVal == nullptr) {
    if (call != nullptr) {
      call->status = HostStatus::Failed;
      call->error = static_cast<long>(DISP_E_TYPEMISMATCH);
      call->detail = "expected an IDispatch value";
    }
    return {};
  }
  return DispatchPtr(value.pdispVal, true);
}

HostStatus ClassifyHresult(long error) {
  switch (static_cast<unsigned long>(static_cast<std::uint32_t>(error))) {
    case kRpcCallRejected:
    case kRpcServerCallRetryLater:
    case kDispatchException:
      return HostStatus::Busy;
    case kClassNotRegistered:
    case kClassString:
    case kMkUnavailable:
    case kObjNotConnected:
    case kRpcDisconnected:
    case kRpcServerDied:
    case kRpcServerDiedDne:
    case kServerExecFailure:
      return HostStatus::Unavailable;
    default:
      return HostStatus::Failed;
  }
}

std::string HResultText(long error) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(static_cast<std::uint32_t>(error)));
  return std::string(buffer);
}

std::string NarrowWide(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}

} // namespace htmlppt
```

- [ ] **Step 6: Add the fake IDispatch**

Create `native/tests/integration/win32/fake_dispatch.h`:

```cpp
#pragma once

#ifdef _WIN32

#include <windows.h>
#include <oaidl.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace htmlppt::test {

// Minimal in-process IDispatch used by the Windows host tests. Properties and
// methods are registered by name; no type library or registration is needed.
class FakeDispatch final : public IDispatch {
 public:
  using Call = std::function<HRESULT(std::vector<VARIANT> args, VARIANT* result,
                                     EXCEPINFO* exception)>;
  using Get = std::function<HRESULT(VARIANT* result, EXCEPINFO* exception)>;
  using Put = std::function<HRESULT(const VARIANT& value)>;

  FakeDispatch() = default;
  ~FakeDispatch() {
    for (auto& entry : members_) VariantClear(&entry.second.value);
  }

  FakeDispatch(const FakeDispatch&) = delete;
  FakeDispatch& operator=(const FakeDispatch&) = delete;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDispatch) {
      *object = static_cast<IDispatch*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1) + 1; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG value = refs_.fetch_sub(1) - 1;
    if (value == 0) delete this;
    return value;
  }

  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
    if (count == nullptr) return E_POINTER;
    *count = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count,
                                          LCID, DISPID* ids) override {
    if (names == nullptr || names[0] == nullptr || ids == nullptr ||
        count != 1) {
      return E_INVALIDARG;
    }
    const std::wstring name(names[0]);
    const auto found = members_.find(name);
    if (found == members_.end()) {
      return static_cast<HRESULT>(DISP_E_UNKNOWNNAME);
    }
    ids[0] = found->second.id;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD flags,
                                   DISPPARAMS* params, VARIANT* result,
                                   EXCEPINFO* exception, UINT*) override {
    if (exception != nullptr) std::memset(exception, 0, sizeof(*exception));
    Member* member = FindById(id);
    if (member == nullptr) {
      return static_cast<HRESULT>(DISP_E_MEMBERNOTFOUND);
    }
    std::vector<VARIANT> args;
    if (params != nullptr) {
      args.reserve(params->cArgs);
      for (UINT i = params->cArgs; i > 0; --i) {
        args.push_back(params->rgvarg[i - 1]);
      }
    }
    if ((flags & DISPATCH_PROPERTYPUT) != 0 ||
        (flags & DISPATCH_PROPERTYPUTREF) != 0) {
      if (args.size() != 1) return static_cast<HRESULT>(DISP_E_BADPARAMCOUNT);
      if (member->put) return member->put(args[0]);
      VariantClear(&member->value);
      VariantInit(&member->value);
      return VariantCopy(&member->value, &args[0]);
    }
    if ((flags & DISPATCH_METHOD) != 0 && member->call) {
      return member->call(std::move(args), result, exception);
    }
    if ((flags & DISPATCH_PROPERTYGET) != 0) {
      if (member->get) return member->get(result, exception);
      if (member->has_value) {
        if (result == nullptr) return S_OK;
        VariantInit(result);
        return VariantCopy(result, &member->value);
      }
      if (member->call) {
        return member->call(std::move(args), result, exception);
      }
    }
    return static_cast<HRESULT>(DISP_E_MEMBERNOTFOUND);
  }

  void SetBool(const std::wstring& name, bool value) {
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_BOOL;
    variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    SetValue(name, variant);
    VariantClear(&variant);
  }

  void SetInt(const std::wstring& name, int value) {
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_I4;
    variant.lVal = value;
    SetValue(name, variant);
    VariantClear(&variant);
  }

  void SetString(const std::wstring& name, const std::wstring& value) {
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_BSTR;
    variant.bstrVal =
        SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
    SetValue(name, variant);
    VariantClear(&variant);
  }

  void SetObject(const std::wstring& name, FakeDispatch* object) {
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_DISPATCH;
    variant.pdispVal = object;  // borrowed; SetValue's VariantCopy AddRefs it
    SetValue(name, variant);
  }

  void SetMethod(const std::wstring& name, Call call) {
    Member& member = members_[name];
    if (member.id == 0) member.id = next_id_++;
    member.call = std::move(call);
  }

  void SetGetter(const std::wstring& name, Get get) {
    Member& member = members_[name];
    if (member.id == 0) member.id = next_id_++;
    member.get = std::move(get);
  }

  void SetPut(const std::wstring& name, Put put) {
    Member& member = members_[name];
    if (member.id == 0) member.id = next_id_++;
    member.put = std::move(put);
  }

 private:
  struct Member {
    DISPID id = 0;
    bool has_value = false;
    VARIANT value{};
    Call call;
    Get get;
    Put put;
  };

  void SetValue(const std::wstring& name, const VARIANT& value) {
    Member& member = members_[name];
    if (member.id == 0) member.id = next_id_++;
    VariantClear(&member.value);
    VariantInit(&member.value);
    (void)VariantCopy(&member.value, &value);
    member.has_value = true;
  }

  Member* FindById(DISPID id) {
    for (auto& entry : members_) {
      if (entry.second.id == id) return &entry.second;
    }
    return nullptr;
  }

  std::atomic<ULONG> refs_{1};
  DISPID next_id_ = 1;
  std::map<std::wstring, Member> members_;
};

} // namespace htmlppt::test

#endif // _WIN32
```

- [ ] **Step 7: Link the COM libraries**

In `native/CMakeLists.txt`, change:

```cmake
  target_link_libraries(htmlppt_win32 PUBLIC htmlppt_core dbghelp)
```

to:

```cmake
  target_link_libraries(htmlppt_win32 PUBLIC htmlppt_core dbghelp ole32 oleaut32)
```

- [ ] **Step 8: Run the Windows gate**

Run the Windows loop. Expected: build succeeds, `htmlppt_win32_tests`
24/24 (22 existing + 2 new), `htmlppt_core_tests` 189/189, CTest 9/9.

Run the Linux gate:

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: 191/191 unit, CTest 8/8. The new files are Windows-only and must not
affect Linux.

- [ ] **Step 9: Commit**

```bash
git add native/src/core/presentation_host.h \
  native/src/platform/win32/com_dispatch.h \
  native/src/platform/win32/com_dispatch.cpp \
  native/tests/integration/win32/fake_dispatch.h \
  native/tests/integration/win32/com_dispatch_test.cpp \
  native/CMakeLists.txt
git commit -m "feat: add win32 com dispatch plumbing"
```

---

### Task 2: WPS Presentation Host and Fake Object Tree

**Files:**
- Create: `native/src/platform/win32/wps_host.h`
- Create: `native/src/platform/win32/wps_host.cpp`
- Create: `native/tests/integration/win32/fake_wps.h`
- Create: `native/tests/integration/win32/wps_host_test.cpp`

**Interfaces:**
- Consumes: `PresentationHost`, `HostResult`, `ShowState`, `HostStatus`; `ComApartment`, `DispatchPtr`, `ComVariant`, `InvokeGet`, `InvokePut`, `InvokeMethod`, `InvokeGetIndexed`, `VariantTo*`, `ClassifyHresult`, `NarrowWide`; `DeckInfoProvider`, `DeckInfo`, `DeckInfoResult`, `DeckSlideInfo`; `AddError`, `AddWarning`; `Clock`, `Duration`, `FakeClock`.
- Produces:
  - `struct WpsHostConfig` with the exact fields in Step 3.
  - `class WpsHost final : public PresentationHost, public DeckInfoProvider` with `Launch`, `StartShow`, `QueryShow`, `ExitShow`, `ResumeShow`, `FocusShow`, `Release`, `Load`, and the injectable `AppFactory`.

- [ ] **Step 1: Write the failing test**

Create `native/tests/integration/win32/wps_host_test.cpp`:

```cpp
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
  WpsHost host(clock, [&fake](long* error) { return FakeFactory(&fake, error); });
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
  WpsHost host(clock, [&fake](long* error) { return FakeFactory(&fake, error); });
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
  WpsHost host(clock, [&fake](long* error) { return FakeFactory(&fake, error); });
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
```

- [ ] **Step 2: Run test to verify it fails**

Run the Windows loop. Expected: build fails because
`platform/win32/wps_host.h` and `fake_wps.h` do not exist.

- [ ] **Step 3: Add the WPS host header**

Create `native/src/platform/win32/wps_host.h`:

```cpp
#pragma once

#ifdef _WIN32

#include <filesystem>
#include <functional>
#include <string>

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
```

- [ ] **Step 4: Add the WPS host implementation**

Create `native/src/platform/win32/wps_host.cpp`:

```cpp
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
                    std::string(name) + " is not an integer");
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
  if (presentation_ && deck_path_ == deck_path) {
    // Reuse the deck that Launch already opened.
  } else {
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
```

- [ ] **Step 5: Add the fake WPS object tree**

Create `native/tests/integration/win32/fake_wps.h`:

```cpp
#pragma once

#ifdef _WIN32

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "fake_dispatch.h"

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
```

- [ ] **Step 6: Run the Windows gate**

Run the Windows loop. Expected: build succeeds, `htmlppt_win32_tests`
27/27 (22 existing + 2 Task 1 + 3 Task 2), `htmlppt_core_tests` 189/189,
CTest 9/9. Run `htmlppt_win32_tests.exe` directly twice to check timing
stability.

- [ ] **Step 7: Run the Linux gate**

Run the Linux build and CTest. Expected: 191/191 unit, CTest 8/8. The new
implementation is Windows-only.

- [ ] **Step 8: Commit**

```bash
git add native/src/platform/win32/wps_host.h \
  native/src/platform/win32/wps_host.cpp \
  native/tests/integration/win32/fake_wps.h \
  native/tests/integration/win32/wps_host_test.cpp
git commit -m "feat: add wps com presentation host"
```

---

### Task 3: CLI WPS Deck Info and Documentation

**Files:**
- Modify: `native/src/app/cli.cpp`
- Modify: `native/README.md`
- Modify: `docs/superpowers/plans/2026-11-15-cpp-watchdog-wps-com-host.md` (as-built)

**Interfaces:**
- Consumes: `WpsHost`, `WpsHostConfig`, `SteadyClock`, `ResolveDeckPath`, `DeckInfoLoad`.
- Produces: automatic WPS deck info for `--validate` and `--resolve` on Windows when `--deck-info` is absent.

- [ ] **Step 1: Add the Windows include**

In `native/src/app/cli.cpp`, after:

```cpp
#include "core/version.h"
```

add:

```cpp
#ifdef _WIN32
#include "platform/win32/wps_host.h"
#endif
```

- [ ] **Step 2: Use the WPS provider when `--deck-info` is absent**

Replace the whole `LoadDeckInfoForCommand` function with:

```cpp
DeckInfoLoad LoadDeckInfoForCommand(const CliOptions& options,
                                    const Config& config) {
  DeckInfoLoad result;
  if (!options.deck_info_path.empty()) {
    JsonDeckInfoProvider provider(options.deck_info_path);
    auto loaded = provider.Load(ResolveDeckPath(config));
    result.issues = std::move(loaded.issues);
    result.info = std::move(loaded.info);
    return result;
  }
#ifdef _WIN32
  SteadyClock clock;
  WpsHostConfig host_config;
  host_config.launch_attempts = 1;
  WpsHost host(clock, host_config);
  auto loaded = host.Load(ResolveDeckPath(config));
  host.Release();
  result.issues = std::move(loaded.issues);
  result.info = std::move(loaded.info);
#endif
  return result;
}
```

- [ ] **Step 3: Build and run both gates**

Linux gate:

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
CHROME_BIN=/home/bingmoe/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome npm test
```

Expected: 191/191 unit, CTest 8/8, `npm test` 3/3.

Windows gate: run the Windows loop. Expected: core 189/189, win32 27/27,
CTest 9/9, `/WX` clean.

- [ ] **Step 4: Document the host and manual acceptance**

Append to `native/README.md`:

````markdown
## WPS COM host (Plan 5A)

`platform/win32/com_dispatch` provides the STA COM apartment, RAII
`IDispatch` pointers, variant conversion, named late-bound calls, and HRESULT
classification. `HostStatus::Busy` covers transient failures including WPS
error 462; `HostStatus::Unavailable` covers missing or disconnected COM
servers; `HostStatus::Failed` is structural.

`platform/win32/wps_host` implements the platform-neutral
`core/presentation_host.h` contract and the existing `DeckInfoProvider`
interface through late-bound `Kwpp.Application` calls:

- `Launch` creates WPS, makes it visible, and opens the deck read-only with
  three attempts.
- `StartShow` sets `StartingSlide=1`, `ShowWithAnimation=True`, calls
  `SlideShowSettings.Run`, and waits up to 2.5 s for a slideshow window.
- `QueryShow` reads `SlideShowWindows.Count` and
  `SlideShowWindow.View.CurrentShowPosition`.
- `ExitShow`, `ResumeShow`, `JumpTo`, and `FocusShow` use the same retry
  timings as the legacy watcher (300 ms between attempts).
- `Load` reads `Slides.Count`, `Slide.Name`, and
  `Shapes.Title.TextFrame.TextRange.Text` and closes a deck opened only for
  diagnostics.

On Windows, `--validate` and `--resolve` use WPS automatically when
`--deck-info` is omitted. `--deck-info` remains the deterministic path for
CI, Linux/WSL, and machines without WPS; if WPS is unavailable the CLI emits
a warning and falls back to the previous limited-resolution behavior.

Manual acceptance on a Windows machine with WPS 12.1:

1. Run `htmlppt-watchdog.exe --resolve --config watcher.json` with a real
   deck and both a numeric and a title-based target. Expected: the table
   resolves every target and prints no deck-info warnings. WPS may open
   briefly; it must not start a slideshow.
2. Run `--validate` with an out-of-range numeric target. Expected: a slide
   range error even without `--deck-info`.
3. Re-run `--resolve --deck-info deck-info.json`. Expected: identical output
   and no WPS process is created.
4. Run `--resolve` with WPS closed and then with WPS unavailable. Expected: a
   warning that names `--deck-info`, not a crash or hang.

`htmlppt_win32_tests` covers the COM helpers and the WPS host with an
in-process fake `IDispatch` tree, so the automated gate does not require WPS.

Windows gate: `htmlppt_core_tests` 189/189, `htmlppt_win32_tests` 27/27,
CTest 9/9, `/WX` clean.
````

- [ ] **Step 5: Run the manual acceptance where possible**

If the current environment has WPS, run manual steps 1-4 and record the
observed results. If it does not, record that manual acceptance is deferred
to the Windows target machine and do not claim it passed.

- [ ] **Step 6: Record the as-built amendment**

Append an `As-Built Amendments (2026-11-15)` section to this plan with:

- the exact Linux and Windows gate counts observed;
- the actual commit hashes;
- any deviation from the code blocks above;
- the manual acceptance status;
- deferred items.

State explicitly that Plan 5 is split into 5A (this plan), 5B (overlay and
focus), 5C (Edge execution, staging, compatibility flags/logs, stop entry),
and 5D (worker business-loop integration and end-to-end acceptance).

- [ ] **Step 7: Commit**

```bash
git add native/src/app/cli.cpp native/README.md \
  docs/superpowers/plans/2026-11-15-cpp-watchdog-wps-com-host.md
git commit -m "feat: use wps deck info in cli diagnostics"
```

---

## Self-Review

### Plan 5A coverage

| Spec requirement | Plan 5A coverage |
|---|---|
| `PresentationHost` interface and WPS implementation | Tasks 1-2 |
| WPS `Kwpp.Application` creation, visible app, read-only deck open | Task 2 `Launch`/`OpenDeck` |
| Slideshow start, exit, resume, jump, focus activation | Task 2 |
| Slide position and show-state polling | Task 2 `QueryShow` |
| Deck title/name reading for `slideTitle`/`slideName` matching | Task 2 `Load` |
| COM 462 transient classification | Tasks 1-2 |
| Retry behavior matching the legacy watcher | Task 2 timings |
| STA COM initialization | Task 1 `ComApartment` |
| `--validate`/`--resolve` WPS deck info on Windows | Task 3 |
| Linux diagnostic build unchanged | All tasks, Windows-only files |
| Automated Windows coverage without WPS | Tasks 1-2 fake dispatch tree |
| Manual acceptance with real WPS | Task 3 README and as-built status |

### Placeholder scan

- No `TBD`, `TODO`, "implement later", or empty steps.
- Every code step contains the code to write.
- Every run step names the exact command and expected result.

### Type consistency

- `HostStatus`, `HostResult`, `ShowState` are defined once in
  `core/presentation_host.h` and used by both COM helpers and `WpsHost`.
- `InvokeGet`, `InvokePut`, `InvokeMethod`, `InvokeGetIndexed` all return
  `HostResult`.
- `DispatchPtr::FromProgId` returns an owning pointer with AddRef already
  accounted for (`add_ref=false` after `CoCreateInstance`).
- `FakeWps::SetDispatchResult` AddRefs the returned object; the parent fake's
  `VariantCopy` also AddRefs stored objects, and `FakeDispatch` releases them
  in its destructor.
- `WpsHost` constructor overloads are unambiguous: `(Clock&)`,
  `(Clock&, WpsHostConfig)`, and `(Clock&, AppFactory, WpsHostConfig = {})`.
- `WpsHost::Load` is the `DeckInfoProvider` override and does not collide with
  `PresentationHost` methods.
- New Windows test count: 22 existing + 2 COM tests + 3 WPS tests = 27.

## Deferred Items

- Plan 5B: overlay window, fade/hold/cover handshake, focus thread, window
  enumeration, foreground unlock, Edge profile window matching.
- Plan 5C: Edge execution, staging `file`/`tree`/`direct`, compatibility flag
  mirror, human-readable log words, legacy launcher, `stop-live.vbs` shim and
  `--stop` integration.
- Plan 5D: worker business loop, config-to-`RunPlan` mapping, state-machine
  event/action wiring, trigger generation filtering, full Windows end-to-end
  acceptance.
- Plan 6: scheduling, priorities, power policy, fault injection.
- Plan 7: config hot reload and native configuration UI.
- Plan 8: diagnostics packaging and release.

## Execution Handoff

Plan 5A is ready. Execute Tasks 1-3 in order; each task ends with both gates
green and a commit. Task 3 records the final gates and as-built.
