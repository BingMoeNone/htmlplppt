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
