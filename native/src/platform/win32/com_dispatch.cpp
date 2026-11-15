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
  if (value.vt == (VT_VARIANT | VT_BYREF) && value.pvarVal != nullptr) {
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
