#include "paths.h"

#include <vector>

namespace sitcom {
namespace {

HMODULE g_dll_module = nullptr;

}  // namespace

void SetDllModule(HMODULE mod) {
  g_dll_module = mod;
}

HMODULE GetDllModule() {
  return g_dll_module;
}

std::wstring GetDllDirectory() {
  HMODULE mod = g_dll_module;
  if (!mod) {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetDllDirectory), &mod);
  }
  if (!mod) {
    return L".";
  }

  std::vector<wchar_t> buf(MAX_PATH);
  DWORD n = GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
  while (n != 0 && n == buf.size()) {
    buf.resize(buf.size() * 2);
    n = GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
  }
  if (n == 0) {
    return L".";
  }

  std::wstring path(buf.data(), n);
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return L".";
  }
  return path.substr(0, slash);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  if (a.back() == L'\\' || a.back() == L'/') {
    return a + b;
  }
  return a + L"\\" + b;
}

bool FileExists(const std::wstring& path) {
  const DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirectory(const std::wstring& path) {
  if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
    return true;
  }
  const DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string out(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), size,
                      nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                       nullptr, 0);
  std::wstring out(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size);
  return out;
}

}  // namespace sitcom
