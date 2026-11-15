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
