#include <windows.h>

#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

std::string ToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int length = static_cast<int>(text.size());
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), size,
                      nullptr, nullptr);
  return result;
}

LRESULT CALLBACK EchoWindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                LPARAM lparam) {
  if (message == WM_DESTROY) return 0;
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void WriteArguments(const std::filesystem::path& output, int argc,
                    wchar_t** argv) {
  std::ofstream stream(output, std::ios::binary);
  for (int index = 0; index < argc; ++index) {
    stream << "arg=" << ToUtf8(argv[index]) << "\n";
  }
  stream.flush();
  stream.close();
  std::ofstream ready(
      std::filesystem::path(output.wstring() + L".ready"), std::ios::binary);
  ready << "ready\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) return 2;
  const std::filesystem::path output(argv[1]);
  const int hold_ms = _wtoi(argv[2]);
  const bool want_window = std::wcscmp(argv[3], L"window") == 0;
  WriteArguments(output, argc, argv);

  if (!want_window) {
    Sleep(static_cast<DWORD>(hold_ms < 0 ? 0 : hold_ms));
    return 0;
  }

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = EchoWindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"HtmlpptArgEchoWindowClass";
  if (RegisterClassW(&window_class) == 0) return 3;

  HWND hwnd = CreateWindowExW(0, window_class.lpszClassName,
                              L"HtmlpptArgEchoWindow", WS_OVERLAPPEDWINDOW,
                              100, 100, 800, 600, nullptr, nullptr,
                              window_class.hInstance, nullptr);
  if (hwnd == nullptr) return 4;
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  UpdateWindow(hwnd);

  const ULONGLONG deadline =
      GetTickCount64() + static_cast<ULONGLONG>(hold_ms < 0 ? 0 : hold_ms);
  while (GetTickCount64() < deadline) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  DestroyWindow(hwnd);
  return 0;
}
