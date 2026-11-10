#include "item_hooks.h"

#include "log.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace sitcom {
namespace {

std::atomic<int> g_acquired{0};
std::atomic<int> g_last_item_id{0};
std::atomic<int> g_last_category{0};
std::atomic<int> g_suppressed{0};

struct InlineHook {
  void* target = nullptr;
  uint8_t* trampoline = nullptr;
  uint8_t stolen[16]{};
  bool active = false;
};

InlineHook g_hook;

bool IsEstusOrAshenRestock(int full_id) {
  if (full_id >= 0x40000096 && full_id <= 0x400000A9) {
    return true;
  }
  if (full_id >= 0x400007E4 && full_id <= 0x400007EE) {
    return true;
  }
  return false;
}

extern "C" {
void* g_item_grant_continue = nullptr;

void ItemGrantNotify(void* /*man*/, void* buffer, int* /*unknown*/) {
  if (!buffer) {
    return;
  }
  const auto buf = reinterpret_cast<const std::uint8_t*>(buffer);
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(buffer, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
    return;
  }

  const int count = *reinterpret_cast<const int*>(buf);
  if (count <= 0 || count > 8) {
    return;
  }

  for (int i = 0; i < count; ++i) {
    const auto entry = buf + 4 + static_cast<std::size_t>(i) * 0x10;
    const int id = *reinterpret_cast<const int*>(entry + 4);
    const int qty = *reinterpret_cast<const int*>(entry + 8);
    if (id == 0) {
      continue;
    }
    g_last_item_id.store(id, std::memory_order_relaxed);
    g_last_category.store(id & 0xF0000000, std::memory_order_relaxed);
    if (IsEstusOrAshenRestock(id)) {
      // Bonfire restock hits this path — do not LogWrite from the grant hook
      // (game thread); just mark suppressed for the ooh filter.
      g_suppressed.fetch_add(1, std::memory_order_relaxed);
      (void)qty;
      continue;
    }
    g_acquired.fetch_add(1, std::memory_order_relaxed);
  }
}

#if defined(__GNUC__)
__attribute__((naked)) void HookedGrantEntry() {
  __asm__ __volatile__(
      "push %%rax\n\t"
      "push %%rcx\n\t"
      "push %%rdx\n\t"
      "push %%r8\n\t"
      "push %%r9\n\t"
      "push %%r10\n\t"
      "push %%r11\n\t"
      "sub $0x20, %%rsp\n\t"
      "call ItemGrantNotify\n\t"
      "add $0x20, %%rsp\n\t"
      "pop %%r11\n\t"
      "pop %%r10\n\t"
      "pop %%r9\n\t"
      "pop %%r8\n\t"
      "pop %%rdx\n\t"
      "pop %%rcx\n\t"
      "pop %%rax\n\t"
      "jmp *g_item_grant_continue(%%rip)\n\t"
      :
      :
      :);
}
#else
#error "Item grant naked hook requires GCC/MinGW"
#endif
}  // extern "C"

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
  return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
         prot == PAGE_EXECUTE_WRITECOPY;
}

std::uintptr_t PatternScan(std::uintptr_t base, std::size_t size, const std::uint8_t* pat,
                           const char* mask) {
  const std::size_t len = std::strlen(mask);
  for (std::size_t i = 0; i + len <= size; ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < len; ++j) {
      if (mask[j] == 'x' && *reinterpret_cast<const std::uint8_t*>(base + i + j) != pat[j]) {
        ok = false;
        break;
      }
    }
    if (ok) {
      return base + i;
    }
  }
  return 0;
}

bool InstallHook(void* target) {
  constexpr SIZE_T kPatch = 14;
  if (!target || !IsExecutable(target)) {
    return false;
  }
  g_hook.trampoline = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!g_hook.trampoline) {
    return false;
  }
  g_hook.target = target;
  std::memcpy(g_hook.stolen, target, kPatch);
  std::memcpy(g_hook.trampoline, g_hook.stolen, kPatch);
  g_hook.trampoline[kPatch] = 0x48;
  g_hook.trampoline[kPatch + 1] = 0xB8;
  const uint64_t ret = reinterpret_cast<uint64_t>(static_cast<uint8_t*>(target) + kPatch);
  std::memcpy(g_hook.trampoline + kPatch + 2, &ret, 8);
  g_hook.trampoline[kPatch + 10] = 0xFF;
  g_hook.trampoline[kPatch + 11] = 0xE0;
  g_item_grant_continue = g_hook.trampoline;

  DWORD old = 0;
  if (!VirtualProtect(target, kPatch, PAGE_EXECUTE_READWRITE, &old)) {
    VirtualFree(g_hook.trampoline, 0, MEM_RELEASE);
    g_hook.trampoline = nullptr;
    g_item_grant_continue = nullptr;
    return false;
  }
  auto* p = static_cast<uint8_t*>(target);
  p[0] = 0x48;
  p[1] = 0xB8;
  const uint64_t d = reinterpret_cast<uint64_t>(&HookedGrantEntry);
  std::memcpy(p + 2, &d, 8);
  p[10] = 0xFF;
  p[11] = 0xE0;
  p[12] = 0x90;
  p[13] = 0x90;
  VirtualProtect(target, kPatch, old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, kPatch);
  g_hook.active = true;
  return true;
}

void RemoveHook() {
  if (!g_hook.active || !g_hook.target) {
    return;
  }
  constexpr SIZE_T kPatch = 14;
  DWORD old = 0;
  if (VirtualProtect(g_hook.target, kPatch, PAGE_EXECUTE_READWRITE, &old)) {
    std::memcpy(g_hook.target, g_hook.stolen, kPatch);
    VirtualProtect(g_hook.target, kPatch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_hook.target, kPatch);
  }
  if (g_hook.trampoline) {
    VirtualFree(g_hook.trampoline, 0, MEM_RELEASE);
    g_hook.trampoline = nullptr;
  }
  g_item_grant_continue = nullptr;
  g_hook.active = false;
}

}  // namespace

bool ItemHooksInit() {
  if (g_hook.active) {
    return true;
  }
  HMODULE game = GetModuleHandleW(L"DarkSoulsIII.exe");
  if (!game) {
    return false;
  }
  MODULEINFO info{};
  if (!GetModuleInformation(GetCurrentProcess(), game, &info, sizeof(info))) {
    return false;
  }
  const auto base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
  const auto size = static_cast<std::size_t>(info.SizeOfImage);

  const std::uint8_t pat[] = {0x8B, 0x02, 0x83, 0xF8, 0x06};
  const char mask[] = "xxxxx";
  const auto inner = PatternScan(base, size, pat, mask);
  if (!inner) {
    LogWrite("item_hooks: MapItemMan grant AOB not found (ooh anim-only)");
    return false;
  }
  const auto hit = inner - 0x2F;
  if (!InstallHook(reinterpret_cast<void*>(hit))) {
    LogWrite("item_hooks: failed to hook MapItemMan grant");
    return false;
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "item_hooks: grant hooked @ 0x%llX (filter estus 0x40000096-A9 / ashen)",
           static_cast<unsigned long long>(hit));
  LogWrite(buf);
  return true;
}

void ItemHooksShutdown() {
  RemoveHook();
  g_acquired.store(0, std::memory_order_relaxed);
  g_suppressed.store(0, std::memory_order_relaxed);
}

int ItemHooksConsumeAcquired() {
  return g_acquired.exchange(0, std::memory_order_relaxed);
}

int ItemHooksConsumeSuppressed() {
  return g_suppressed.exchange(0, std::memory_order_relaxed);
}

int ItemHooksLastItemId() {
  return g_last_item_id.load(std::memory_order_relaxed);
}

int ItemHooksLastCategory() {
  return g_last_category.load(std::memory_order_relaxed);
}

}  // namespace sitcom
