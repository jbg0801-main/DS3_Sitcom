#include "log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace sitcom {
namespace {

std::mutex g_mu;
HANDLE g_file = INVALID_HANDLE_VALUE;
bool g_enabled = false;

bool OpenLogFile(const std::wstring& path) {
  if (g_file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
  }
  g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  return g_file != INVALID_HANDLE_VALUE;
}

void WriteRaw(const std::string& line) {
  if (g_file == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  FlushFileBuffers(g_file);
}

}  // namespace

bool LogBootstrap(const std::wstring& preferred_path) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_enabled = true;
  if (OpenLogFile(preferred_path)) {
    WriteRaw("---- sitcom bootstrap ----\r\n");
    return true;
  }
  return false;
}

void LogInit(const std::wstring& log_path, bool enabled) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!enabled) {
    // Keep bootstrap file handle if we already opened one; just stop writing new lines
    // unless bootstrap wanted continuous logs. Prefer honouring config: close if disabled.
    if (g_file != INVALID_HANDLE_VALUE) {
      WriteRaw("---- sitcom logging disabled by config ----\r\n");
      CloseHandle(g_file);
      g_file = INVALID_HANDLE_VALUE;
    }
    g_enabled = false;
    return;
  }
  g_enabled = true;
  if (!OpenLogFile(log_path)) {
    g_enabled = false;
    return;
  }
  WriteRaw("---- sitcom log start ----\r\n");
}

void LogWrite(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_enabled || g_file == INVALID_HANDLE_VALUE) {
    return;
  }
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char prefix[64];
  snprintf(prefix, sizeof(prefix), "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond,
           st.wMilliseconds);
  std::string out = std::string(prefix) + line + "\r\n";
  WriteRaw(out);
}

bool LogEnabled() {
  return g_enabled;
}

}  // namespace sitcom
