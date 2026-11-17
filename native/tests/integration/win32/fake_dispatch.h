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
    // VariantCopy takes a non-const source in the MinGW headers and a const one
    // in the MSVC headers; the cast keeps the fake compiling on both.
    (void)VariantCopy(&member.value, const_cast<VARIANT*>(&value));
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
