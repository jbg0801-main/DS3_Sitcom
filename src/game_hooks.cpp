#include "game_hooks.h"

#include "log.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace sitcom {
namespace {

constexpr std::uintptr_t kImageBase = 0x140000000ULL;

constexpr std::uintptr_t kRvaIsLocalPlayer = 0x14088CFF0ULL - kImageBase;
constexpr std::uintptr_t kRvaWorldChrMan = 0x14477FDB8ULL - kImageBase;
constexpr std::uintptr_t kRvaMapNameState = 0x144763258ULL - kImageBase;
constexpr std::uintptr_t kRvaUseItem = 0x1405BA4C0ULL - kImageBase;
constexpr std::uintptr_t kRvaHandleUseItem = 0x1409B1620ULL - kImageBase;
constexpr std::uintptr_t kRvaTryStartUseItem = 0x140880DC0ULL - kImageBase;
constexpr std::uintptr_t kRvaLookupGoods = 0x140E2F200ULL - kImageBase;
constexpr std::uintptr_t kRvaQueueMenuEvent = 0x140765F30ULL - kImageBase;
constexpr std::uintptr_t kRvaApplyDamageToHp = 0x1409C48C0ULL - kImageBase;
constexpr std::uintptr_t kRvaChrKill = 0x140888FC0ULL - kImageBase;
constexpr std::uint32_t kMapNameTitleEvent = 8;
constexpr std::uint32_t kBossGaugeBase = 0x1AF8;
constexpr std::uint32_t kBossGaugeStride = 0x98;
constexpr std::uint32_t kPlaceNameIdOff = 0x1A38;       // pending / just written
constexpr std::uint32_t kShownPlaceNameIdOff = 0x1A3C;  // last shown title id
// MOD_FINDINGS: ladder act byte at *(*(chr+0x50)+0x48)+0x2CC.
constexpr std::uint32_t kLadderActOff = 0x2CC;

std::atomic<int> g_empty_estus{0};
std::atomic<int> g_area_title{0};
std::atomic<int> g_hit{0};
std::atomic<int> g_death{0};
std::atomic<int> g_boss_cheer{0};
std::atomic<int> g_boss_applause{0};
std::atomic<int> g_ladder_fall{0};
std::atomic<std::int32_t> g_last_place_id{-1};
std::atomic<bool> g_death_from_hit{false};

std::uintptr_t g_mod_base = 0;
using IsLocalPlayer_fn = std::uint8_t (*)(void* chr);
IsLocalPlayer_fn g_is_local = nullptr;
// SoloParamRepository EquipParamGoods lookup: holder{id, unk, row*}.
using LookupGoods_fn = void (*)(void* holder, std::uint32_t goods_id);
LookupGoods_fn g_lookup_goods = nullptr;

struct InlineHook {
  void* target = nullptr;
  std::uint8_t* trampoline = nullptr;
  std::uint8_t stolen[32]{};
  int stolen_len = 0;
  bool active = false;
};

InlineHook g_hk_use_item;
InlineHook g_hk_handle_use_item;
InlineHook g_hk_try_start_use;
InlineHook g_hk_queue_menu;
InlineHook g_hk_apply_dmg;
InlineHook g_hk_chr_kill;
std::int32_t g_last_ladder_act = -999;
bool g_boss_slot_active[3]{};
bool g_boss_slots_seeded = false;

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

bool IsReadable(const void* p, std::size_t n = 8) {
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
  const auto start = reinterpret_cast<std::uintptr_t>(p);
  const auto end = start + n;
  const auto region = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  return end <= region;
}

void* Va(std::uintptr_t rva) {
  return reinterpret_cast<void*>(g_mod_base + rva);
}

// Write a 14-byte register-preserving absolute jmp: FF 25 00 00 00 00 <imm64>
// (jmp qword ptr [rip+0]). Avoids clobbering rax — required when stolen bytes are
// `mov rax, rsp` style prologues (Chr_OnBehaviorEvent).
void WriteAbsJmp(std::uint8_t* at, std::uint64_t dest) {
  at[0] = 0xFF;
  at[1] = 0x25;
  at[2] = 0x00;
  at[3] = 0x00;
  at[4] = 0x00;
  at[5] = 0x00;
  std::memcpy(at + 6, &dest, 8);
}

// Absolute jmp is 14 bytes; stolen_len must be >=14 and end on an insn boundary.
bool InstallHook(InlineHook* hk, void* target, void* detour, int stolen_len) {
  constexpr int kJmp = 14;
  if (!hk || !target || !detour || !IsExecutable(target) || stolen_len < kJmp ||
      stolen_len > static_cast<int>(sizeof(hk->stolen))) {
    return false;
  }
  if (hk->active) {
    return true;
  }
  hk->trampoline = static_cast<std::uint8_t*>(
      VirtualAlloc(nullptr, 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!hk->trampoline) {
    return false;
  }
  hk->target = target;
  hk->stolen_len = stolen_len;
  std::memcpy(hk->stolen, target, static_cast<std::size_t>(stolen_len));
  std::memcpy(hk->trampoline, hk->stolen, static_cast<std::size_t>(stolen_len));
  WriteAbsJmp(hk->trampoline + stolen_len,
              reinterpret_cast<std::uint64_t>(static_cast<std::uint8_t*>(target) + stolen_len));

  DWORD old = 0;
  if (!VirtualProtect(target, static_cast<SIZE_T>(stolen_len), PAGE_EXECUTE_READWRITE, &old)) {
    VirtualFree(hk->trampoline, 0, MEM_RELEASE);
    hk->trampoline = nullptr;
    return false;
  }
  auto* p = static_cast<std::uint8_t*>(target);
  WriteAbsJmp(p, reinterpret_cast<std::uint64_t>(detour));
  for (int i = kJmp; i < stolen_len; ++i) {
    p[i] = 0x90;
  }
  VirtualProtect(target, static_cast<SIZE_T>(stolen_len), old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, static_cast<SIZE_T>(stolen_len));
  hk->active = true;
  return true;
}

void RemoveHook(InlineHook* hk) {
  if (!hk || !hk->active || !hk->target || hk->stolen_len <= 0) {
    return;
  }
  DWORD old = 0;
  if (VirtualProtect(hk->target, static_cast<SIZE_T>(hk->stolen_len), PAGE_EXECUTE_READWRITE,
                     &old)) {
    std::memcpy(hk->target, hk->stolen, static_cast<std::size_t>(hk->stolen_len));
    VirtualProtect(hk->target, static_cast<SIZE_T>(hk->stolen_len), old, &old);
    FlushInstructionCache(GetCurrentProcess(), hk->target, static_cast<SIZE_T>(hk->stolen_len));
  }
  if (hk->trampoline) {
    VirtualFree(hk->trampoline, 0, MEM_RELEASE);
    hk->trampoline = nullptr;
  }
  hk->active = false;
  hk->target = nullptr;
  hk->stolen_len = 0;
}

bool IsLocalPlayer(void* chr) {
  if (!chr || !g_mod_base) {
    return false;
  }
  // Prefer WorldChrMan+0x80 — avoids calling into game code from hot hooks.
  const auto slot = reinterpret_cast<void**>(Va(kRvaWorldChrMan));
  if (IsReadable(slot)) {
    void* wcm = *slot;
    if (wcm && IsReadable(static_cast<char*>(wcm) + 0x80)) {
      if (*reinterpret_cast<void**>(static_cast<char*>(wcm) + 0x80) == chr) {
        return true;
      }
      // Slot populated but not this chr — do not fall through to game call.
      return false;
    }
  }
  if (g_is_local && IsExecutable(reinterpret_cast<const void*>(g_is_local))) {
    return g_is_local(chr) != 0;
  }
  return false;
}

std::int32_t ReadChrHp(void* chr) {
  if (!chr || !IsReadable(static_cast<char*>(chr) + 0x1F90)) {
    return -1;
  }
  void* mods = *reinterpret_cast<void**>(static_cast<char*>(chr) + 0x1F90);
  if (!mods || !IsReadable(static_cast<char*>(mods) + 0x18)) {
    return -1;
  }
  void* data = *reinterpret_cast<void**>(static_cast<char*>(mods) + 0x18);
  if (!data || !IsReadable(static_cast<char*>(data) + 0xD8)) {
    return -1;
  }
  return *reinterpret_cast<std::int32_t*>(static_cast<char*>(data) + 0xD8);
}

std::int32_t ReadChrHandle(void* chr) {
  if (!chr || !IsReadable(static_cast<char*>(chr) + 8)) {
    return -1;
  }
  return *reinterpret_cast<std::int32_t*>(static_cast<char*>(chr) + 8);
}

// EquipParamGoods row id (strip 0x40000000 inventory encoding if present).
int EstusGoodsRow(int goods_id) {
  if (goods_id & 0xF0000000) {
    return goods_id & 0x00FFFFFF;
  }
  return goods_id;
}

void* LookupEquipParamGoodsRow(int goods_id) {
  if (!g_lookup_goods || goods_id < 0) {
    return nullptr;
  }
  alignas(16) std::uint8_t holder[16]{};
  *reinterpret_cast<std::uint32_t*>(holder) = 0xFFFFFFFFu;
  g_lookup_goods(holder, static_cast<std::uint32_t>(EstusGoodsRow(goods_id)));
  void* row = *reinterpret_cast<void**>(holder + 8);
  if (!row || !IsReadable(static_cast<char*>(row) + 0x69, 1)) {
    return nullptr;
  }
  return row;
}

// MOD_FINDINGS: +0x69 SuppleItemType 1=HP Estus, 2=Ashen;
// +0x45 bit7 isSuppleItem = empty flask; +0x46 bit0 isFullSuppleItem = charged.
// Runtime: ChrCtrl_UseItem only runs for charged drinks (odd rows). Empty shake
// skips UseItem inside ChrAction_HandleUseItem when start==0 && info2!=1.
bool IsEmptyEstusGoods(int goods_id) {
  void* row = LookupEquipParamGoodsRow(goods_id);
  if (row) {
    const auto type = *reinterpret_cast<std::uint8_t*>(static_cast<char*>(row) + 0x69);
    if (type != 1 && type != 2) {
      return false;
    }
    const auto b45 = *reinterpret_cast<std::uint8_t*>(static_cast<char*>(row) + 0x45);
    if ((b45 & 0x80) != 0) {
      return true;  // empty flask row
    }
    const auto b46 = *reinterpret_cast<std::uint8_t*>(static_cast<char*>(row) + 0x46);
    if ((b46 & 0x01) != 0) {
      return false;  // charged flask row
    }
  }
  // Fallback when param lookup fails: shipped empty ids are even in flask bands.
  const int row_id = EstusGoodsRow(goods_id);
  if ((row_id >= 150 && row_id <= 169) || (row_id >= 190 && row_id <= 209)) {
    return (row_id % 2) == 0;
  }
  return false;
}

void NoteEmptyEstusIfLocal(void* chr, int goods_id) {
  // Backup only: empty EquipParamGoods row (+0x45 bit7). Primary empty signal is
  // ItemDrinkEmpty anim in events.cpp — the "0-charge HandleUseItem" path falsely
  // fired on the last charged drink (1→0), not on a true empty use.
  if (!IsLocalPlayer(chr) || !IsEmptyEstusGoods(goods_id)) {
    return;
  }
  g_empty_estus.fetch_add(1, std::memory_order_relaxed);
  char buf[96];
  snprintf(buf, sizeof(buf), "game_hooks: empty Estus goods=0x%X row=%d", goods_id,
           EstusGoodsRow(goods_id));
  LogWrite(buf);
}

bool BossGaugeHasHandle(std::int32_t handle) {
  if (handle == -1 || !g_mod_base) {
    return false;
  }
  const auto state_slot = reinterpret_cast<void**>(Va(kRvaMapNameState));
  if (!IsReadable(state_slot)) {
    return false;
  }
  void* state = *state_slot;
  if (!state) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    const auto base = static_cast<char*>(state) + kBossGaugeBase + i * kBossGaugeStride;
    if (!IsReadable(base + 8)) {
      continue;
    }
    const auto name_id = *reinterpret_cast<std::int32_t*>(base);
    const auto slot_handle = *reinterpret_cast<std::int32_t*>(base + 4);
    if (name_id != -1 && slot_handle == handle) {
      return true;
    }
  }
  return false;
}

void NoteBossKillIfGauged(void* chr) {
  const auto handle = ReadChrHandle(chr);
  if (BossGaugeHasHandle(handle)) {
    g_boss_applause.fetch_add(1, std::memory_order_relaxed);
  }
}

void* MapNameState() {
  if (!g_mod_base) {
    return nullptr;
  }
  const auto state_slot = reinterpret_cast<void**>(Va(kRvaMapNameState));
  if (!IsReadable(state_slot)) {
    return nullptr;
  }
  void* state = *state_slot;
  if (!state) {
    return nullptr;
  }
  return state;
}

void NotePlaceId() {
  void* state = MapNameState();
  if (!state || !IsReadable(static_cast<char*>(state) + kPlaceNameIdOff)) {
    return;
  }
  const auto id = *reinterpret_cast<std::int32_t*>(static_cast<char*>(state) + kPlaceNameIdOff);
  g_last_place_id.store(id, std::memory_order_relaxed);
}

std::int32_t ReadShownPlaceNameId() {
  void* state = MapNameState();
  if (!state || !IsReadable(static_cast<char*>(state) + kShownPlaceNameIdOff)) {
    return -1;
  }
  return *reinterpret_cast<std::int32_t*>(static_cast<char*>(state) + kShownPlaceNameIdOff);
}

using UseItem_fn = std::uint8_t (*)(void*, int, std::uint32_t*, int, char);
using HandleUseItem_fn = void (*)(void*, void*);
using TryStartUseItem_fn = std::uint8_t (*)(void*, void*, void*);
// Prologue: ecx = menu_event_id, dl = flags; returns al success.
using QueueMenu_fn = std::uint8_t (*)(std::int32_t, std::uint8_t);
using ApplyDmg_fn = void (*)(void*, void*, std::int32_t, std::int32_t, std::uint8_t, std::uint8_t);
using ChrKill_fn = void (*)(void*, std::uint8_t);
// Prologue: cmp byte ptr [r8+8], 1 — args in R8.
using MediumDestroy_fn = void (*)(void*, void*, void*);

// Backup only — charged drinks dominate this path; empty usually never arrives.
std::uint8_t HookedUseItem(void* chr_action_ctrl, int goods_id, std::uint32_t* use_item_info,
                           int slot_or_flags, char start_behavior) {
  auto* orig = reinterpret_cast<UseItem_fn>(g_hk_use_item.trampoline);
  void* chr = chr_action_ctrl
                  ? reinterpret_cast<char*>(chr_action_ctrl) - 0x10E8
                  : nullptr;
  NoteEmptyEstusIfLocal(chr, goods_id);
  return orig(chr_action_ctrl, goods_id, use_item_info, slot_or_flags, start_behavior);
}

void HookedHandleUseItem(void* action, void* args) {
  auto* orig = reinterpret_cast<HandleUseItem_fn>(g_hk_handle_use_item.trampoline);
  if (action && IsReadable(static_cast<char*>(action) + 0x10)) {
    void* chr = *reinterpret_cast<void**>(static_cast<char*>(action) + 0x10);
    if (chr && IsReadable(static_cast<char*>(chr) + 0x11C0, 4)) {
      const int goods_id = *reinterpret_cast<std::int32_t*>(static_cast<char*>(chr) + 0x11C0);
      NoteEmptyEstusIfLocal(chr, goods_id);
    }
  }
  orig(action, args);
}

// Shared anim-start for item use (including empty shake when request carries empty goods).
std::uint8_t HookedTryStartUseItem(void* request, void* action_slot, void* chr) {
  auto* orig = reinterpret_cast<TryStartUseItem_fn>(g_hk_try_start_use.trampoline);
  if (request && IsReadable(static_cast<char*>(request) + 8, 4)) {
    // BuildUseItemEzStateRequest: goods id at request+4 (param_1[1]).
    const int goods_id = *reinterpret_cast<std::int32_t*>(static_cast<char*>(request) + 4);
    NoteEmptyEstusIfLocal(chr, goods_id);
  }
  return orig(request, action_slot, chr);
}

std::uint8_t HookedQueueMenuEvent(std::int32_t menu_event_id, std::uint8_t flags) {
  auto* orig = reinterpret_cast<QueueMenu_fn>(g_hk_queue_menu.trampoline);
  if (menu_event_id == static_cast<std::int32_t>(kMapNameTitleEvent)) {
    NotePlaceId();
    g_area_title.fetch_add(1, std::memory_order_relaxed);
  }
  return orig(menu_event_id, flags);
}

void HookedApplyDamageToHp(void* damage_module, void* damage_info, std::int32_t max_damage,
                           std::int32_t applied_damage, std::uint8_t a5, std::uint8_t a6) {
  auto* orig = reinterpret_cast<ApplyDmg_fn>(g_hk_apply_dmg.trampoline);
  void* chr = nullptr;
  if (damage_module && IsReadable(static_cast<char*>(damage_module) + 8)) {
    chr = *reinterpret_cast<void**>(static_cast<char*>(damage_module) + 8);
  }
  const bool local = IsLocalPlayer(chr);
  const std::int32_t hp_before = chr ? ReadChrHp(chr) : -1;
  orig(damage_module, damage_info, max_damage, applied_damage, a5, a6);
  if (applied_damage <= 0) {
    return;
  }
  const std::int32_t hp_after = chr ? ReadChrHp(chr) : -1;
  // Boss killing blow: ApplyDamageToHp crosses 0 and does not call Chr_Kill.
  if (!local) {
    if (hp_before > 0 && hp_after <= 0) {
      NoteBossKillIfGauged(chr);
    }
    return;
  }
  // Local player hit / death.
  if (hp_before > 0 && hp_after <= 0) {
    g_death_from_hit.store(true, std::memory_order_relaxed);
    g_death.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_hit.fetch_add(1, std::memory_order_relaxed);
  }
}

void HookedChrKill(void* chr, std::uint8_t death_kind) {
  auto* orig = reinterpret_cast<ChrKill_fn>(g_hk_chr_kill.trampoline);
  if (IsLocalPlayer(chr)) {
    if (!g_death_from_hit.exchange(false, std::memory_order_relaxed)) {
      g_death.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    NoteBossKillIfGauged(chr);
  }
  orig(chr, death_kind);
}

// MOD_FINDINGS: bar is on when slot+0 (name) and +4 (handle) are both != -1.
// Event_SetBossGauge trampoline was armed but never produced cheers in-game
// (Iudex bar up, counter stayed 0). Polling the live slots is the reliable edge.
void PollBossGaugeSlots() {
  void* state = MapNameState();
  if (!state) {
    return;
  }
  bool any_readable = false;
  bool active[3]{};
  for (int i = 0; i < 3; ++i) {
    auto* base = static_cast<char*>(state) + kBossGaugeBase + static_cast<std::size_t>(i) * kBossGaugeStride;
    if (!IsReadable(base + 8)) {
      continue;
    }
    any_readable = true;
    const auto name_id = *reinterpret_cast<std::int32_t*>(base);
    const auto handle = *reinterpret_cast<std::int32_t*>(base + 4);
    active[i] = name_id != -1 && handle != -1;
  }
  if (!any_readable) {
    return;
  }
  if (!g_boss_slots_seeded) {
    for (int i = 0; i < 3; ++i) {
      g_boss_slot_active[i] = active[i];
    }
    g_boss_slots_seeded = true;
    return;
  }
  for (int i = 0; i < 3; ++i) {
    if (active[i] && !g_boss_slot_active[i]) {
      g_boss_cheer.fetch_add(1, std::memory_order_relaxed);
    } else if (!active[i] && g_boss_slot_active[i]) {
      // Slot cleared — victory, fail, or warp. events.cpp gates on applause_armed_.
      g_boss_applause.fetch_add(1, std::memory_order_relaxed);
    }
    g_boss_slot_active[i] = active[i];
  }
}

// Poll ladder act byte. OnBehaviorEvent uses `mov rax,rsp` / lea-rbp frame
// setup — a CALL trampoline corrupts that and crashes on long paths (first
// bonfire light). Polling avoids those hooks entirely.
void PollLadderAct() {
  const auto slot = reinterpret_cast<void**>(Va(kRvaWorldChrMan));
  if (!IsReadable(slot)) {
    return;
  }
  void* wcm = *slot;
  if (!wcm || !IsReadable(static_cast<char*>(wcm) + 0x80)) {
    return;
  }
  void* chr = *reinterpret_cast<void**>(static_cast<char*>(wcm) + 0x80);
  if (!chr || !IsReadable(static_cast<char*>(chr) + 0x50)) {
    return;
  }
  void* mid = *reinterpret_cast<void**>(static_cast<char*>(chr) + 0x50);
  if (!mid || !IsReadable(static_cast<char*>(mid) + 0x48)) {
    return;
  }
  void* ladder = *reinterpret_cast<void**>(static_cast<char*>(mid) + 0x48);
  if (!ladder || !IsReadable(static_cast<char*>(ladder) + kLadderActOff, 1)) {
    return;
  }
  const auto act =
      static_cast<std::int32_t>(*reinterpret_cast<std::int8_t*>(static_cast<char*>(ladder) + kLadderActOff));
  const std::int32_t prev = g_last_ladder_act;
  g_last_ladder_act = act;
  if (prev == act) {
    return;
  }
  if (act == 21 || act == 22 || act == 23) {
    g_ladder_fall.fetch_add(1, std::memory_order_relaxed);
  }
}

void LogHook(const char* name, void* addr, bool ok) {
  char buf[128];
  snprintf(buf, sizeof(buf), "game_hooks: %s %s @ 0x%llX", name, ok ? "armed" : "FAILED",
           static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addr)));
  LogWrite(buf);
}

}  // namespace

bool GameHooksInit() {
  HMODULE game = GetModuleHandleW(L"DarkSoulsIII.exe");
  if (!game) {
    LogWrite("game_hooks: DarkSoulsIII.exe not loaded");
    return false;
  }
  MODULEINFO info{};
  if (!GetModuleInformation(GetCurrentProcess(), game, &info, sizeof(info))) {
    return false;
  }
  g_mod_base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
  g_is_local = reinterpret_cast<IsLocalPlayer_fn>(Va(kRvaIsLocalPlayer));
  g_lookup_goods = reinterpret_cast<LookupGoods_fn>(Va(kRvaLookupGoods));

  struct Job {
    const char* name;
    InlineHook* hk;
    std::uintptr_t rva;
    void* detour;
    int steal;
  };
  // Steal sizes end on instruction boundaries (objdump of DarkSoulsIII.exe).
  // NOT hooked (CALL trampolines unsafe / unused):
  // - OnBehaviorEvent*: `mov rax,rsp` frame setup; crashed BonfireInitialize.
  // - UseItem/HandleUseItem/TryStart: empty flask is anim-primary; goods lookup
  //   from these hooks also crashed first bonfire light.
  // - QueueMenu: never observed menu_event_id==8 in logs; wipe uses place id.
  // Ladder/boss bar/defeat: polled. Empty flask: drink anim.
  // MediumBossDestroy not hooked — same CALL-trampoline issues; defeat uses
  // gauged ApplyDamageToHp kill + slot-clear poll (gated by applause_armed_).
  const Job jobs[] = {
      {"ApplyDamageToHp", &g_hk_apply_dmg, kRvaApplyDamageToHp,
       reinterpret_cast<void*>(&HookedApplyDamageToHp), 15},
      {"Chr_Kill", &g_hk_chr_kill, kRvaChrKill, reinterpret_cast<void*>(&HookedChrKill), 20},
  };

  int ok_n = 0;
  for (const auto& j : jobs) {
    void* addr = Va(j.rva);
    const bool ok = InstallHook(j.hk, addr, j.detour, j.steal);
    LogHook(j.name, addr, ok);
    if (ok) {
      ++ok_n;
    }
  }
  LogWrite("game_hooks: installed " + std::to_string(ok_n) + "/" +
           std::to_string(sizeof(jobs) / sizeof(jobs[0])));
  return ok_n > 0;
}

void GameHooksShutdown() {
  RemoveHook(&g_hk_apply_dmg);
  RemoveHook(&g_hk_chr_kill);
  g_is_local = nullptr;
  g_lookup_goods = nullptr;
  g_mod_base = 0;
  g_last_ladder_act = -999;
  g_boss_slots_seeded = false;
  g_boss_slot_active[0] = g_boss_slot_active[1] = g_boss_slot_active[2] = false;
}

int GameHooksConsumeEmptyEstus() {
  return g_empty_estus.exchange(0, std::memory_order_relaxed);
}
int GameHooksConsumeAreaTitle() {
  return g_area_title.exchange(0, std::memory_order_relaxed);
}
int GameHooksConsumeHit() {
  return g_hit.exchange(0, std::memory_order_relaxed);
}
int GameHooksConsumeDeath() {
  return g_death.exchange(0, std::memory_order_relaxed);
}
int GameHooksConsumeBossCheer() {
  PollBossGaugeSlots();
  return g_boss_cheer.exchange(0, std::memory_order_relaxed);
}
int GameHooksConsumeBossApplause() {
  PollBossGaugeSlots();
  return g_boss_applause.exchange(0, std::memory_order_relaxed);
}
int GameHooksConsumeLadderFall() {
  PollLadderAct();
  return g_ladder_fall.exchange(0, std::memory_order_relaxed);
}
std::int32_t GameHooksLastPlaceNameId() {
  return g_last_place_id.load(std::memory_order_relaxed);
}

std::int32_t GameHooksReadShownPlaceNameId() {
  return ReadShownPlaceNameId();
}

}  // namespace sitcom
