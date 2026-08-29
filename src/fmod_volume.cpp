#include "fmod_volume.h"

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

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
using FnEventStart = FmodResult(__cdecl*)(void* event, int paused, void** instance);
using FnEventGetInfo = FmodResult(__cdecl*)(void* event, void* info, char* name, int namelen);

FnGetCategory g_get_category = nullptr;
FnGetVolume g_get_volume = nullptr;
FnCppUpdate g_real_update = nullptr;
FnEventStart g_real_event_start = nullptr;
FnEventGetInfo g_event_get_info = nullptr;

std::atomic<EventSystem*> g_event_system{nullptr};
std::atomic<EventCategory*> g_se_category{nullptr};
std::atomic<float> g_se_volume{1.f};
std::atomic<bool> g_se_valid{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_captured{false};
std::atomic<bool> g_want_hook{false};
std::atomic<bool> g_probe_transitions{false};
std::atomic<bool> g_area_transition_pending{false};
char g_area_transition_name[128]{};

std::mutex g_hook_mu;
std::mutex g_area_mu;

struct InlineHook {
  void* target = nullptr;
  uint8_t stolen[16]{};
  bool active = false;
};

InlineHook g_update_hook;
InlineHook g_event_start_hook;

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

bool Strieq(const char* hay, const char* needle) {
  if (!hay || !needle) {
    return false;
  }
  const size_t nlen = std::strlen(needle);
  if (nlen == 0) {
    return true;
  }
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (i < nlen && p[i] &&
           std::tolower(static_cast<unsigned char>(p[i])) ==
               std::tolower(static_cast<unsigned char>(needle[i]))) {
      ++i;
    }
    if (i == nlen) {
      return true;
    }
  }
  return false;
}

bool LooksLikeAreaTransitionEvent(const char* name) {
  if (!name || !name[0]) {
    return false;
  }
  static const char* kNeedles[] = {
      "location",   "area_title", "areatitle", "area_name", "map_title",  "maptitle",
      "place_name", "placename",  "field_name", "landmark",  "region_name", "title_card",
      "titlecard",  "map_name",   "mapname",    "area_card", "location_name",
      "map_in",     "mapin",      "area_in",   "areain",    "enter",       "discovery",
  };
  for (const char* needle : kNeedles) {
    if (Strieq(name, needle)) {
      return true;
    }
  }
  if ((name[0] == 'f' || name[0] == 'F' || name[0] == 's' || name[0] == 'S') &&
      std::strchr(name, '_') &&
      (Strieq(name, "title") || Strieq(name, "location") || Strieq(name, "land"))) {
    return true;
  }
  return false;
}

void NoteAreaTransitionEvent(const char* name) {
  if (!name || !name[0]) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_area_mu);
    std::snprintf(g_area_transition_name, sizeof(g_area_transition_name), "%s", name);
    g_area_transition_pending.store(true, std::memory_order_release);
  }
  LogWrite(std::string("fmod: area-transition event start — ") + name);
}

void MaybeLogFmodEventStart(const char* name) {
  if (!g_probe_transitions.load(std::memory_order_relaxed) || !name || !name[0]) {
    return;
  }
  static std::mutex log_mu;
  static std::unordered_set<std::string> seen;
  std::lock_guard<std::mutex> lock(log_mu);
  if (seen.insert(name).second) {
    LogWrite(std::string("fmod: event start probe — ") + name);
  }
}

void RestoreHook(InlineHook* hook) {
  if (!hook || !hook->active || !hook->target) {
    return;
  }
  constexpr SIZE_T kPatch = 14;
  DWORD old = 0;
  if (VirtualProtect(hook->target, kPatch, PAGE_EXECUTE_READWRITE, &old)) {
    std::memcpy(hook->target, hook->stolen, kPatch);
    VirtualProtect(hook->target, kPatch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hook->target, kPatch);
  }
  hook->active = false;
}

void RestoreUpdatePrologue() { RestoreHook(&g_update_hook); }

bool InstallPermanentHook(InlineHook* hook, void* target, void* detour, void** real_out) {
  constexpr SIZE_T kPatch = 14;
  if (!hook || !target || !detour || !IsExecutable(target)) {
    return false;
  }
  if (hook->active && hook->target == target) {
    return true;
  }
  const auto* bytes = static_cast<const uint8_t*>(target);
  if (bytes[0] == 0xE9 || bytes[0] == 0xCC || bytes[0] == 0xC3) {
    return false;
  }

  hook->target = target;
  std::memcpy(hook->stolen, target, kPatch);
  if (real_out) {
    *real_out = target;
  }

  DWORD old = 0;
  if (!VirtualProtect(target, kPatch, PAGE_EXECUTE_READWRITE, &old)) {
    return false;
  }
  auto* p = static_cast<uint8_t*>(target);
  p[0] = 0x48;
  p[1] = 0xB8;
  const uint64_t d = reinterpret_cast<uint64_t>(detour);
  std::memcpy(p + 2, &d, 8);
  p[10] = 0xFF;
  p[11] = 0xE0;
  p[12] = 0x90;
  p[13] = 0x90;
  VirtualProtect(target, kPatch, old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, kPatch);
  hook->active = true;
  return true;
}

bool InstallOneShotUpdateHook(void* target);

FmodResult __cdecl HookedEventStart(void* event, int paused, void** instance) {
  if (!g_real_event_start) {
    return 0;
  }
  const FmodResult result = g_real_event_start(event, paused, instance);
  if (event) {
    char name[256]{};
    bool named = false;
    if (g_event_get_info) {
      named = g_event_get_info(event, nullptr, name, static_cast<int>(sizeof(name))) == 0 &&
              name[0];
    }
    if (named) {
      MaybeLogFmodEventStart(name);
      if (LooksLikeAreaTransitionEvent(name)) {
        NoteAreaTransitionEvent(name);
      }
    } else if (g_probe_transitions.load(std::memory_order_relaxed)) {
      static std::atomic<int> unnamed{0};
      const int n = unnamed.fetch_add(1, std::memory_order_relaxed);
      if (n < 20) {
        char buf[80];
        snprintf(buf, sizeof(buf), "fmod: event start unnamed #%d event=%p", n,
                 event);
        LogWrite(buf);
      }
    }
  }
  return result;
}

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

  g_event_get_info =
      reinterpret_cast<FnEventGetInfo>(GetProcAddress(fmod, "FMOD_Event_GetInfo"));
  void* event_start = reinterpret_cast<void*>(GetProcAddress(fmod, "FMOD_Event_Start"));
  if (!event_start) {
    event_start = reinterpret_cast<void*>(
        GetProcAddress(fmod, "?start@Event@FMOD@@QEAA?AW4FMOD_RESULT@@XZ"));
  }

  bool installed = false;
  bool event_hooked = false;
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    g_want_hook.store(true, std::memory_order_relaxed);
    installed = InstallOneShotUpdateHook(update);
    if (!installed) {
      g_want_hook.store(false, std::memory_order_relaxed);
    }
    if (event_start && g_event_get_info) {
      void* real = nullptr;
      event_hooked =
          InstallPermanentHook(&g_event_start_hook, event_start, reinterpret_cast<void*>(&HookedEventStart),
                               &real);
      if (event_hooked) {
        g_real_event_start = reinterpret_cast<FnEventStart>(real);
      }
    }
  }
  if (!installed) {
    LogWrite("fmod: failed to install one-shot update hook");
    return false;
  }

  g_ready.store(true, std::memory_order_relaxed);
  LogWrite("fmod: EventSystem::update hook armed (audio-thread SE capture)");
  if (event_hooked) {
    LogWrite("fmod: FMOD_Event_Start hook armed (area-transition SFX capture)");
  } else if (!g_event_get_info) {
    LogWrite("fmod: FMOD_Event_GetInfo missing (area-transition SFX hook disabled)");
  } else {
    LogWrite("fmod: FMOD_Event_Start export missing (area-transition SFX hook disabled)");
  }
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
  bool was_update = false;
  bool was_event = false;
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    was_update = g_update_hook.active;
    was_event = g_event_start_hook.active;
    RestoreUpdatePrologue();
    RestoreHook(&g_event_start_hook);
    g_real_event_start = nullptr;
  }
  if (was_update) {
    LogWrite("fmod: EventSystem::update hook removed");
  }
  if (was_event) {
    LogWrite("fmod: FMOD_Event_Start hook removed");
  }
  g_ready.store(false, std::memory_order_relaxed);
  g_event_system.store(nullptr, std::memory_order_relaxed);
  g_se_category.store(nullptr, std::memory_order_relaxed);
  g_se_valid.store(false, std::memory_order_relaxed);
  g_area_transition_pending.store(false, std::memory_order_relaxed);
}

void FmodSetProbeTransitions(bool enabled) {
  g_probe_transitions.store(enabled, std::memory_order_relaxed);
}

bool FmodConsumeAreaTransition(char* out_name, int out_name_cap) {
  if (!g_area_transition_pending.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  if (out_name && out_name_cap > 0) {
    std::lock_guard<std::mutex> lock(g_area_mu);
    std::snprintf(out_name, static_cast<size_t>(out_name_cap), "%s", g_area_transition_name);
  }
  return true;
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
