#include "fmod_volume.h"

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace sitcom {
namespace {

// DSR uses FMOD C++ EventSystem::update. A permanent 14-byte trampoline hook
// captured SE volume then crashed on the resume (instruction boundary).
//
// One-shot hook: on update(), read SE via C GetCategory/GetVolume (audio thread),
// restore the original prologue, call the real function, then re-arm so the next
// update() captures again. The worker thread never calls FMOD APIs — Event is not
// thread-safe, and polling GetVolume from the sitcom thread crashed at the title
// menu once BGM started.

using FmodResult = int;
using EventSystem = void;
using EventCategory = void;

using FnGetCategory = FmodResult(__cdecl*)(EventSystem*, const char*, EventCategory**);
using FnGetVolume = FmodResult(__cdecl*)(EventCategory*, float*);
using FnCppUpdate = FmodResult(__cdecl*)(EventSystem* self);

FnGetCategory g_get_category = nullptr;
FnGetVolume g_get_volume = nullptr;
FnCppUpdate g_real_update = nullptr;

std::atomic<EventSystem*> g_event_system{nullptr};
std::atomic<EventCategory*> g_se_category{nullptr};
std::atomic<float> g_se_volume{1.f};
std::atomic<bool> g_se_valid{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_captured{false};
std::atomic<bool> g_want_hook{false};

std::mutex g_hook_mu;

struct InlineHook {
  void* target = nullptr;
  uint8_t stolen[16]{};
  bool active = false;
};

InlineHook g_update_hook;

bool IsReadable(const void* p, size_t n = 8) {
  if (!p) {
    return false;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) {
    return false;
  }
  if (mbi.State != MEM_COMMIT) {
    return false;
  }
  const DWORD prot = mbi.Protect & 0xFF;
  if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) {
    return false;
  }
  const auto start = reinterpret_cast<const uint8_t*>(p);
  const auto end = start + n;
  const auto reg_start = static_cast<const uint8_t*>(mbi.BaseAddress);
  const auto reg_end = reg_start + mbi.RegionSize;
  return start >= reg_start && end <= reg_end;
}

bool IsExecutable(const void* p) {
  if (!p) {
    return false;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) {
    return false;
  }
  if (mbi.State != MEM_COMMIT) {
    return false;
  }
  const DWORD prot = mbi.Protect & 0xFF;
  return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_WRITECOPY ||
         prot == PAGE_EXECUTE_READWRITE;
}

void NoteSeVolume(EventCategory* cat, float vol) {
  if (vol < 0.f) {
    vol = 0.f;
  }
  if (vol > 1.f) {
    vol = 1.f;
  }
  if (cat) {
    g_se_category.store(cat, std::memory_order_relaxed);
  }
  const bool first = !g_se_valid.load(std::memory_order_relaxed);
  const float prev = g_se_volume.load(std::memory_order_relaxed);
  g_se_volume.store(vol, std::memory_order_relaxed);
  g_se_valid.store(true, std::memory_order_relaxed);
  if (first || std::fabs(prev - vol) > 0.01f) {
    char buf[96];
    snprintf(buf, sizeof(buf), "fmod: SE volume=%.3f", vol);
    LogWrite(buf);
  }
}

bool TryReadSe(EventSystem* es) {
  if (!es || !g_get_category || !g_get_volume || !IsReadable(es)) {
    return false;
  }
  EventCategory* cat = nullptr;
  if (g_get_category(es, "SE", &cat) != 0 || !cat || !IsReadable(cat)) {
    cat = nullptr;
    if (g_get_category(es, "master/SE", &cat) != 0 || !cat || !IsReadable(cat)) {
      return false;
    }
  }
  float vol = 1.f;
  if (g_get_volume(cat, &vol) != 0) {
    return false;
  }
  g_event_system.store(es, std::memory_order_relaxed);
  NoteSeVolume(cat, vol);
  return true;
}

void RestoreUpdatePrologue() {
  if (!g_update_hook.active || !g_update_hook.target) {
    return;
  }
  constexpr SIZE_T kPatch = 14;
  DWORD old = 0;
  if (VirtualProtect(g_update_hook.target, kPatch, PAGE_EXECUTE_READWRITE, &old)) {
    std::memcpy(g_update_hook.target, g_update_hook.stolen, kPatch);
    VirtualProtect(g_update_hook.target, kPatch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_update_hook.target, kPatch);
  }
  g_update_hook.active = false;
}

bool InstallOneShotUpdateHook(void* target);

FmodResult __cdecl HookedCppUpdate(EventSystem* self) {
  const bool first = !g_captured.load(std::memory_order_relaxed);
  if (first) {
    LogWrite(self ? "fmod: one-shot update capture" : "fmod: one-shot update (null this)");
  }
  if (self) {
    if (TryReadSe(self)) {
      g_captured.store(true, std::memory_order_relaxed);
    } else if (first) {
      LogWrite("fmod: GetCategory(SE) failed on EventSystem::update this");
    }
  }

  // Restore real prologue, call the live function, then re-arm. GetVolume stays
  // on the audio thread so the sitcom worker never calls into FMOD Event.
  void* target = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    target = g_update_hook.target;
    RestoreUpdatePrologue();
  }
  if (!target) {
    return 0;
  }
  g_real_update = reinterpret_cast<FnCppUpdate>(target);
  const FmodResult result = g_real_update(self);
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    if (g_want_hook.load(std::memory_order_relaxed)) {
      InstallOneShotUpdateHook(target);
    }
  }
  return result;
}

bool InstallOneShotUpdateHook(void* target) {
  constexpr SIZE_T kPatch = 14;
  if (!target || !IsExecutable(target)) {
    return false;
  }
  if (g_update_hook.active && g_update_hook.target == target) {
    return true;
  }
  const auto* bytes = static_cast<const uint8_t*>(target);
  if (bytes[0] == 0xE9 || bytes[0] == 0xCC || bytes[0] == 0xC3) {
    return false;
  }

  g_update_hook.target = target;
  std::memcpy(g_update_hook.stolen, target, kPatch);
  g_real_update = reinterpret_cast<FnCppUpdate>(target);

  DWORD old = 0;
  if (!VirtualProtect(target, kPatch, PAGE_EXECUTE_READWRITE, &old)) {
    return false;
  }
  auto* p = static_cast<uint8_t*>(target);
  p[0] = 0x48;
  p[1] = 0xB8;
  const uint64_t d = reinterpret_cast<uint64_t>(&HookedCppUpdate);
  std::memcpy(p + 2, &d, 8);
  p[10] = 0xFF;
  p[11] = 0xE0;
  p[12] = 0x90;
  p[13] = 0x90;
  VirtualProtect(target, kPatch, old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, kPatch);
  g_update_hook.active = true;
  return true;
}

bool ResolveAndHook() {
  HMODULE fmod = GetModuleHandleW(L"fmod_event64.dll");
  if (!fmod) {
    return false;
  }

  g_get_category =
      reinterpret_cast<FnGetCategory>(GetProcAddress(fmod, "FMOD_EventSystem_GetCategory"));
  g_get_volume =
      reinterpret_cast<FnGetVolume>(GetProcAddress(fmod, "FMOD_EventCategory_GetVolume"));
  if (!g_get_category || !g_get_volume) {
    LogWrite("fmod: missing C GetCategory/GetVolume");
    return false;
  }

  // Export dump showed EventSystem::update (not EventSystemI). Prefer that.
  void* update = reinterpret_cast<void*>(
      GetProcAddress(fmod, "?update@EventSystem@FMOD@@QEAA?AW4FMOD_RESULT@@XZ"));
  if (!update) {
    update = reinterpret_cast<void*>(
        GetProcAddress(fmod, "?update@EventSystemI@FMOD@@UEAA?AW4FMOD_RESULT@@XZ"));
  }
  if (!update) {
    LogWrite("fmod: EventSystem::update export missing");
    return false;
  }

  bool installed = false;
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    g_want_hook.store(true, std::memory_order_relaxed);
    installed = InstallOneShotUpdateHook(update);
    if (!installed) {
      g_want_hook.store(false, std::memory_order_relaxed);
    }
  }
  if (!installed) {
    LogWrite("fmod: failed to install one-shot update hook");
    return false;
  }

  g_ready.store(true, std::memory_order_relaxed);
  LogWrite("fmod: EventSystem::update hook armed (audio-thread SE capture)");
  return true;
}

}  // namespace

bool FmodVolumeInit() {
  if (g_ready.load(std::memory_order_relaxed) || g_captured.load(std::memory_order_relaxed)) {
    return g_ready.load(std::memory_order_relaxed) || g_se_valid.load(std::memory_order_relaxed);
  }
  if (!GetModuleHandleW(L"fmod_event64.dll")) {
    return false;
  }
  return ResolveAndHook();
}

void FmodVolumeShutdown() {
  g_want_hook.store(false, std::memory_order_relaxed);
  bool was_hooked = false;
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    was_hooked = g_update_hook.active;
    RestoreUpdatePrologue();
  }
  if (was_hooked) {
    LogWrite("fmod: EventSystem::update hook removed");
  }
  g_ready.store(false, std::memory_order_relaxed);
  g_event_system.store(nullptr, std::memory_order_relaxed);
  g_se_category.store(nullptr, std::memory_order_relaxed);
  g_se_valid.store(false, std::memory_order_relaxed);
}

bool FmodTryGetSfxVolume(float* out_volume) {
  if (!g_ready.load(std::memory_order_relaxed) && !g_captured.load(std::memory_order_relaxed)) {
    FmodVolumeInit();
  }
  // Cache only — never GetVolume/GetCategory from the worker thread.
  if (!g_se_valid.load(std::memory_order_relaxed)) {
    static int miss = 0;
    if ((++miss % 100) == 1) {
      char buf[128];
      snprintf(buf, sizeof(buf), "fmod: waiting (captured=%d ready=%d)",
               g_captured.load() ? 1 : 0, g_ready.load() ? 1 : 0);
      LogWrite(buf);
    }
    return false;
  }
  if (out_volume) {
    *out_volume = g_se_volume.load(std::memory_order_relaxed);
  }
  return true;
}

}  // namespace sitcom
