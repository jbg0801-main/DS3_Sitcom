#include "dinput8_proxy.h"

#include "log.h"
#include "paths.h"

#include <string>
#include <vector>

namespace {

DirectInput8Create_t g_original_direct_input8_create = nullptr;
std::vector<HMODULE> g_chain_modules;
HMODULE g_system_dinput = nullptr;

std::wstring Trim(std::wstring s) {
  while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' ||
                        s.front() == L'\n')) {
    s.erase(s.begin());
  }
  while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' ||
                        s.back() == L'\n')) {
    s.pop_back();
  }
  return s;
}

void SplitCommaList(const std::wstring& raw, std::vector<std::wstring>* out) {
  std::wstring cur;
  for (wchar_t c : raw) {
    if (c == L',' || c == L';') {
      cur = Trim(cur);
      if (!cur.empty()) {
        out->push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  cur = Trim(cur);
  if (!cur.empty()) {
    out->push_back(cur);
  }
}

void AppendFromIni(const std::wstring& ini_path, std::vector<std::wstring>* out) {
  if (!sitcom::FileExists(ini_path)) {
    return;
  }
  wchar_t buf[1024];
  GetPrivateProfileStringW(L"chainload", L"dlls", L"", buf, 1024, ini_path.c_str());
  SplitCommaList(buf, out);
  // Single-DLL alias used by some other mods' docs.
  GetPrivateProfileStringW(L"chainload", L"dll", L"", buf, 1024, ini_path.c_str());
  const std::wstring one = Trim(buf);
  if (!one.empty()) {
    out->push_back(one);
  }
}

void AppendFromTxt(const std::wstring& txt_path, std::vector<std::wstring>* out) {
  if (!sitcom::FileExists(txt_path)) {
    return;
  }
  HANDLE f = CreateFileW(txt_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024) {
    CloseHandle(f);
    return;
  }
  std::string bytes(static_cast<size_t>(sz.QuadPart), '\0');
  DWORD read = 0;
  if (!ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) {
    CloseHandle(f);
    return;
  }
  CloseHandle(f);
  bytes.resize(read);

  std::string line;
  auto flush = [&]() {
    // Strip comments (# or ;).
    const auto hash = line.find_first_of("#;");
    if (hash != std::string::npos) {
      line.resize(hash);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
      line.pop_back();
    }
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    if (i < line.size()) {
      out->push_back(sitcom::Utf8ToWide(line.substr(i)));
    }
    line.clear();
  };
  for (char c : bytes) {
    if (c == '\n') {
      flush();
    } else {
      line.push_back(c);
    }
  }
  flush();
}

std::wstring ResolveChainPath(const std::wstring& dll_dir, const std::wstring& entry) {
  if (entry.empty()) {
    return {};
  }
  // Absolute or drive-relative.
  if (entry.size() >= 2 && ((entry[1] == L':') || (entry[0] == L'\\' && entry[1] == L'\\'))) {
    return entry;
  }
  // Leading slash → still relative to game/DLL dir (ModEngine style "\othermod.dll").
  std::wstring name = entry;
  while (!name.empty() && (name.front() == L'\\' || name.front() == L'/')) {
    name.erase(name.begin());
  }
  return sitcom::JoinPath(dll_dir, name);
}

bool SameModulePath(HMODULE ours, const std::wstring& path) {
  if (!ours || path.empty()) {
    return false;
  }
  wchar_t self[MAX_PATH];
  const DWORD n = GetModuleFileNameW(ours, self, MAX_PATH);
  if (!n) {
    return false;
  }
  return _wcsicmp(self, path.c_str()) == 0;
}

bool LoadSystemDinput(HMODULE* out_module, DirectInput8Create_t* out_create) {
  wchar_t sys[MAX_PATH];
  if (!GetSystemDirectoryW(sys, MAX_PATH)) {
    return false;
  }
  std::wstring path(sys);
  path += L"\\dinput8.dll";

  HMODULE mod = LoadLibraryW(path.c_str());
  if (!mod) {
    sitcom::LogWrite("proxy: failed to load system dinput8.dll");
    return false;
  }

  auto fn = reinterpret_cast<DirectInput8Create_t>(GetProcAddress(mod, "DirectInput8Create"));
  if (!fn) {
    sitcom::LogWrite("proxy: system DirectInput8Create missing");
    FreeLibrary(mod);
    return false;
  }

  g_system_dinput = mod;
  *out_module = mod;
  *out_create = fn;
  g_original_direct_input8_create = fn;
  sitcom::LogWrite("proxy: system dinput8 loaded");
  return true;
}

}  // namespace

bool LoadRealDinput8(HMODULE* out_module, DirectInput8Create_t* out_create) {
  g_chain_modules.clear();
  g_system_dinput = nullptr;
  g_original_direct_input8_create = nullptr;

  const std::wstring dll_dir = sitcom::GetDllDirectory();
  const std::wstring sitcom_dir = sitcom::JoinPath(dll_dir, L"sitcom");

  // Early log so chain decisions appear even before the worker opens config logging.
  sitcom::LogBootstrap(sitcom::JoinPath(sitcom_dir, L"sitcom.log"));

  std::vector<std::wstring> entries;
  AppendFromIni(sitcom::JoinPath(sitcom_dir, L"config.ini"), &entries);
  AppendFromTxt(sitcom::JoinPath(sitcom_dir, L"chainload.txt"), &entries);

  DirectInput8Create_t chain_create = nullptr;
  HMODULE chain_di_mod = nullptr;

  for (const auto& entry : entries) {
    const std::wstring path = ResolveChainPath(dll_dir, entry);
    if (path.empty()) {
      continue;
    }
    if (SameModulePath(sitcom::GetDllModule(), path)) {
      sitcom::LogWrite("proxy: chainload skip self " + sitcom::WideToUtf8(path));
      continue;
    }
    if (!sitcom::FileExists(path)) {
      sitcom::LogWrite("proxy: chainload missing " + sitcom::WideToUtf8(path));
      continue;
    }

    HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod) {
      sitcom::LogWrite("proxy: chainload LoadLibrary failed " + sitcom::WideToUtf8(path) +
                       " err=" + std::to_string(GetLastError()));
      continue;
    }
    g_chain_modules.push_back(mod);
    sitcom::LogWrite("proxy: chainloaded " + sitcom::WideToUtf8(path));

    if (!chain_create) {
      auto fn = reinterpret_cast<DirectInput8Create_t>(GetProcAddress(mod, "DirectInput8Create"));
      if (fn) {
        chain_create = fn;
        chain_di_mod = mod;
        sitcom::LogWrite("proxy: DirectInput8Create forwarded to " + sitcom::WideToUtf8(path));
      }
    }
  }

  if (chain_create) {
    *out_module = chain_di_mod;
    *out_create = chain_create;
    g_original_direct_input8_create = chain_create;
    return true;
  }

  return LoadSystemDinput(out_module, out_create);
}

void UnloadRealDinput8(HMODULE /*module*/) {
  g_original_direct_input8_create = nullptr;
  for (auto it = g_chain_modules.rbegin(); it != g_chain_modules.rend(); ++it) {
    if (*it) {
      FreeLibrary(*it);
    }
  }
  g_chain_modules.clear();
  if (g_system_dinput) {
    FreeLibrary(g_system_dinput);
    g_system_dinput = nullptr;
  }
}

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion,
                                                                   REFIID riidltf, LPVOID* ppvOut,
                                                                   LPUNKNOWN punkOuter) {
  if (g_original_direct_input8_create) {
    return g_original_direct_input8_create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
  }
  return E_FAIL;
}
