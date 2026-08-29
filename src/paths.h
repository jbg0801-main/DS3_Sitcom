#pragma once

#include <string>
#include <windows.h>

namespace sitcom {

void SetDllModule(HMODULE mod);
HMODULE GetDllModule();
std::wstring GetDllDirectory();
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
bool FileExists(const std::wstring& path);
bool EnsureDirectory(const std::wstring& path);
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);

}  // namespace sitcom
