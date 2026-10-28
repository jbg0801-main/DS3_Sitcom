#include "game_state.h"

#include "event_flags.h"
#include "log.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>

namespace sitcom {
namespace {

EventFlags g_event_flags;

bool IsReadable(std::uintptr_t ptr) {
  if (!ptr) {
    return false;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(ptr), &mbi, sizeof(mbi))) {
    return false;
  }
  if (mbi.State != MEM_COMMIT) {
    return false;
  }
  const DWORD p = mbi.Protect & 0xFF;
  return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_EXECUTE_READ ||
         p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY;
}

template <typename T>
T ReadT(std::uintptr_t ptr, T fallback = T{}) {
  return IsReadable(ptr) ? *reinterpret_cast<T*>(ptr) : fallback;
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

std::uintptr_t ResolveRip(std::uintptr_t insn, int rel_off, int insn_size) {
  const auto rel = *reinterpret_cast<std::int32_t*>(insn + rel_off);
  return insn + insn_size + rel;
}

std::uintptr_t ScanRipSlot(std::uintptr_t base, std::size_t size, const std::uint8_t* pat,
                           const char* mask, int rel_off, int insn_size) {
  const auto hit = PatternScan(base, size, pat, mask);
  return hit ? ResolveRip(hit, rel_off, insn_size) : 0;
}

std::uintptr_t ScanRipFirst(std::uintptr_t base, std::size_t size, int rel_off, int insn_size,
                            const std::uint8_t* pat, const char* mask) {
  if (std::strlen(mask) == 0) {
    return 0;
  }
  return ScanRipSlot(base, size, pat, mask, rel_off, insn_size);
}

// cheer = fight-active flag, defeat = boss-dead flag (SoulSplitter / EMEVD notes).
constexpr BossPair kBossPairs[] = {
    {14000801, 14000800},  // Iudex Gundyr
    {13000801, 13000800},  // Vordt
    {13100801, 13100800},  // Curse-Rotted Greatwood
    {13300851, 13300850},  // Crystal Sage
    {13300801, 13300800},  // Abyss Watchers
    {13500801, 13500800},  // Deacons of the Deep
    {13800801, 13800800},  // High Lord Wolnir
    {13800831, 13800830},  // Old Demon King
    {13700851, 13700850},  // Pontiff Sulyvahn
    {13900801, 13900800},  // Yhorm
    {13700801, 13700800},  // Aldrich
    {13000891, 13000890},  // Dancer
    {13010801, 13010800},  // Dragonslayer Armour
    {13000831, 13000830},  // Oceiros
    {14000831, 14000830},  // Champion Gundyr
    {13410831, 13410830},  // Twin Princes
    {13200801, 13200800},  // Ancient Wyvern
    {13200851, 13200850},  // Nameless King
    {14100801, 14100800},  // Soul of Cinder
    {14500801, 14500800},  // Sister Friede
    {14500861, 14500860},  // Gravetender & Greatwolf
    {15000801, 15000800},  // Demon Prince
    {15100801, 15100800},  // Halflight
    {15100851, 15100850},  // Darkeater Midir
    {15110801, 15110800},  // Slave Knight Gael
};

std::uintptr_t ReadPlayerIns(std::uintptr_t world_chr_slot) {
  if (!world_chr_slot || !IsReadable(world_chr_slot)) {
    return 0;
  }
  const auto world_chr = ReadT<std::uintptr_t>(world_chr_slot);
  if (!world_chr) {
    return 0;
  }
  return ReadT<std::uintptr_t>(world_chr + 0x80);
}

bool IsEstusFlaskItemId(std::int32_t item_id) {
  if (item_id >= 0x40000096 && item_id <= 0x400000A9) {
    return true;
  }
  if (item_id >= 0x400000BE && item_id <= 0x400000D3) {
    return true;
  }
  if (item_id >= 0x400007E4 && item_id <= 0x400007EE) {
    return true;
  }
  return false;
}

void ReadEstusCharges(GameSnapshot& s, std::uintptr_t /*game_data_man_slot*/) {
  // Remaining drink count is not exposed in TGA PlayerGameData; empty-flask uses anim detection.
  s.estus_charges = -1;
  s.estus_charges_valid = false;
}

std::uintptr_t Deref(std::uintptr_t addr) {
  if (!IsReadable(addr)) {
    return 0;
  }
  return ReadT<std::uintptr_t>(addr);
}

bool SaneFadeRemain(float v) {
  return std::isfinite(v) && v >= -0.01f && v <= 30.f;
}

bool SaneFadeAlpha(float v) {
  return std::isfinite(v) && v >= -0.01f && v <= 1.05f;
}

// CE pointer path: read between offsets, add on the last.
std::uintptr_t ResolveCePointerField(std::uintptr_t slot, const std::uint32_t* offs,
                                     std::size_t count) {
  if (!slot || !offs || count == 0 || !IsReadable(slot)) {
    return 0;
  }
  std::uintptr_t addr = ReadT<std::uintptr_t>(slot);
  if (!addr) {
    return 0;
  }
  for (std::size_t i = 0; i + 1 < count; ++i) {
    if (!IsReadable(addr + offs[i])) {
      return 0;
    }
    addr = ReadT<std::uintptr_t>(addr + offs[i]);
    if (!addr) {
      return 0;
    }
  }
  if (!IsReadable(addr + offs[count - 1])) {
    return 0;
  }
  return addr + offs[count - 1];
}

// Parent chain reads, then add embedded struct + field offset (no deref at +28).
std::uintptr_t ResolveCePointerEmbedded(std::uintptr_t slot, const std::uint32_t* chain,
                                      std::size_t chain_count, std::uint32_t field_off) {
  if (!slot || !chain || chain_count == 0 || !IsReadable(slot)) {
    return 0;
  }
  std::uintptr_t addr = ReadT<std::uintptr_t>(slot);
  if (!addr) {
    return 0;
  }
  for (std::size_t i = 0; i < chain_count; ++i) {
    if (!IsReadable(addr + chain[i])) {
      return 0;
    }
    addr = ReadT<std::uintptr_t>(addr + chain[i]);
    if (!addr) {
      return 0;
    }
  }
  if (!IsReadable(addr + field_off)) {
    return 0;
  }
  return addr + field_off;
}

// TGA CT v3.4 FADE → Title RemainTiem offsets (CE last→first): 38, 28, 0, 8, 8.
// +0x28 is an embedded panel (no deref); fields: RemainTiem +0x38, CurrentColor.A +0x1C.
// MapIn RemainTiem shares the panel at +0xF8.
bool ReadSprjFadeTitleFade(std::uintptr_t sprj_fade_slot, float* title_remain, float* title_alpha,
                           float* mapin_remain, float* mapin_alpha) {
  if (!title_remain || !title_alpha || !mapin_remain || !mapin_alpha || !sprj_fade_slot) {
    return false;
  }

  *title_remain = 0.f;
  *title_alpha = 0.f;
  *mapin_remain = 0.f;
  *mapin_alpha = 0.f;

  const auto try_panel = [&](std::uintptr_t panel) -> bool {
    if (!panel || !IsReadable(panel + 0xF8)) {
      return false;
    }
    const float tr = ReadT<float>(panel + 0x38);
    const float ta = ReadT<float>(panel + 0x1C);
    const float mr = ReadT<float>(panel + 0xF8);
    const float ma = ReadT<float>(panel + 0xDC);
    const auto sane_or_zero = [](float v, float max_v) {
      return v == 0.f || (std::isfinite(v) && v >= -0.01f && v <= max_v);
    };
    if (!sane_or_zero(tr, 30.f) || !sane_or_zero(ta, 1.05f) || !sane_or_zero(mr, 30.f) ||
        !sane_or_zero(ma, 1.05f)) {
      return false;
    }
    *title_remain = tr;
    *title_alpha = ta;
    *mapin_remain = mr;
    *mapin_alpha = ma;
    return true;
  };

  // Primary: TGA chain [[[SprjFade]+8]+8]+0 + 0x28.
  const std::uint32_t chain_long[] = {0x8, 0x8, 0x0};
  if (try_panel(ResolveCePointerEmbedded(sprj_fade_slot, chain_long, 3, 0x28))) {
    return true;
  }
  // Fallback without the +0 deref.
  const std::uint32_t chain_short[] = {0x8, 0x8};
  if (try_panel(ResolveCePointerEmbedded(sprj_fade_slot, chain_short, 2, 0x28))) {
    return true;
  }
  return false;
}

}  // namespace

bool GameState::ResolveBases() {
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

  // WorldChrMan — TGA / SoulMemory
  const std::uint8_t pat_w1[] = {0x48, 0x8B, 0x1D, 0, 0, 0, 0, 0x48, 0x8B, 0xF9, 0x48,
                                 0x85, 0xDB, 0,    0,    0x8B, 0x11, 0x85, 0xD2, 0, 0, 0x8D};
  const char mask_w1[] = "xxx????xxxxxx??xxxx??x";
  world_chr_ptr_ = ScanRipSlot(base, size, pat_w1, mask_w1, 3, 7);

  if (!world_chr_ptr_) {
    const std::uint8_t pat_w2[] = {
        0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x45, 0x33, 0xC0, 0x48, 0x8D, 0x55, 0xE7, 0xE8,
        0,    0,    0,    0, 0x0F, 0x2F, 0x73, 0x70, 0x72, 0x0D, 0xF3, 0,    0,    0,    0,
        0,    0,    0,    0, 0x0F, 0x11, 0x43, 0x70};
    const char mask_w2[] = "xxx????xxxxxxx????xxxxxxxxx????xxxx";
    world_chr_ptr_ = ScanRipSlot(base, size, pat_w2, mask_w2, 3, 7);
  }

  // GameDataMan — SoulMemory / TGA
  const std::uint8_t pat_gdm1[] = {0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x4C, 0x8D, 0x44, 0x24, 0x40,
                                   0x45, 0x33, 0xC9, 0x48, 0x8B, 0xD3, 0x40, 0x88, 0x74, 0x24,
                                   0x28, 0x44, 0x88, 0x74, 0x24, 0x20};
  const char mask_gdm1[] = "xxx????xxxxxxxxxxxxxxxxxxxx";
  game_data_man_ptr_ = ScanRipSlot(base, size, pat_gdm1, mask_gdm1, 3, 7);

  if (!game_data_man_ptr_) {
    const std::uint8_t pat_gdm2[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0, 0,
                                     0x48, 0x8B, 0x40, 0, 0xC3};
    const char mask_gdm2[] = "xxx????xxx??xxx?x";
    game_data_man_ptr_ = ScanRipSlot(base, size, pat_gdm2, mask_gdm2, 3, 7);
  }

  // GameMan — title flow (IsTitleStart @ +0xB1D); try TGA variants in order
  {
    const std::uint8_t pat_gm1[] = {0x48, 0x8B, 0, 0, 0, 0, 0, 0x04, 0x89, 0x48, 0x28, 0xC3};
    const std::uint8_t pat_gm2[] = {0x48, 0x8B, 0, 0, 0, 0, 0, 0x89, 0x48, 0x28, 0xC3};
    const std::uint8_t pat_gm3[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x89, 0x48, 0x28, 0xC3};
    const struct {
      const std::uint8_t* pat;
      const char* mask;
      const char* label;
    } kGameManPatterns[] = {
        {pat_gm1, "xx?????xxxxx", "TGA+0x04"},
        {pat_gm2, "xx????xxxxx", "no-0x04"},
        {pat_gm3, "xxx????xxxxx", "mov-rax"},
    };
    for (const auto& spec : kGameManPatterns) {
      game_man_ptr_ = ScanRipFirst(base, size, 3, 7, spec.pat, spec.mask);
      if (game_man_ptr_) {
        LogWrite(std::string("game_state: GameMan resolved via pattern ") + spec.label);
        break;
      }
    }
  }

  // NewMenuSystem — gameplay menu state (+0x88 != 3 in-game)
  const std::uint8_t pat_nms1[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x74, 0,
                                   0x80, 0xB8, 0,    0,    0,    0,    0,    0,    0x0F, 0x95,
                                   0xC0, 0xC3};
  const char mask_nms1[] = "xxx????xxx?xx????xxx";
  new_menu_system_ptr_ = ScanRipSlot(base, size, pat_nms1, mask_nms1, 3, 7);

  if (!new_menu_system_ptr_) {
    const std::uint8_t pat_nms2[] = {0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x48, 0x8B, 0x7C, 0x24, 0x20,
                                     0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x85, 0xC9};
    const char mask_nms2[] = "xxx????xxxxxxxxxx";
    new_menu_system_ptr_ = ScanRipSlot(base, size, pat_nms2, mask_nms2, 3, 7);
  }

  // MenuMan — DS3 uses flags[i32] at +0x32 (not DSR byte at +0x104). Banner type 8 = Current Location.
  // Mask must be exactly 20 chars: 48 8B 0D + disp4 + 33 C0 48 39 81 + disp4 + 0F 95 C0 C3
  const std::uint8_t pat_mm[] = {0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x33, 0xC0, 0x48, 0x39, 0x81,
                                 0,    0,    0,    0, 0x0F, 0x95, 0xC0, 0xC3};
  const char mask_mm[] = "xxx????xxxxx????xxxx";
  menu_man_ptr_ = ScanRipFirst(base, size, 3, 7, pat_mm, mask_mm);

  // Loading flag byte (resolved RIP target is the flag itself)
  const std::uint8_t pat_ld[] = {0xC6, 0x05, 0, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x84, 0xC0,
                                 0x0F, 0x94, 0xC0, 0xE9};
  const char mask_ld[] = "xx????x????xxxxxx";
  loading_ptr_ = ScanRipSlot(base, size, pat_ld, mask_ld, 2, 7);

  // FieldArea — event flags context + current world block (+0x20)
  const std::uint8_t pat_fa[] = {0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x48, 0x85, 0xC9, 0x74, 0x26,
                                 0x44, 0x8B};
  const char mask_fa[] = "xxx????xxxxxxx";
  field_area_ptr_ = ScanRipSlot(base, size, pat_fa, mask_fa, 3, 7);

  // SprjFade — area title card fade (TGA pointer chain; try TGA AOB then SoulSplitter)
  {
    const std::uint8_t pat_sf_tga[] = {0x48, 0x89, 0x7C, 0x24, 0,    0x48, 0x8B, 0xF9, 0xE8, 0,
                                       0,    0,    0,    0,    0x84, 0xC0, 0x74, 0,    0x48, 0x8B, 0x0D};
    const char mask_sf_tga[] = "xxxx?xxxx????xx?xxx";
    const auto hit_tga = PatternScan(base, size, pat_sf_tga, mask_sf_tga);
    if (hit_tga) {
      sprj_fade_ptr_ = ResolveRip(hit_tga + 17, 3, 7);
    }
    if (!sprj_fade_ptr_) {
      const std::uint8_t pat_sf[] = {0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x4C, 0x8D, 0x4C, 0x24, 0x38,
                                     0x4C, 0x8D, 0x44, 0x24, 0x48, 0x33, 0xD2};
      const char mask_sf[] = "xxx????xxxxxxxxxx";
      sprj_fade_ptr_ = ScanRipSlot(base, size, pat_sf, mask_sf, 3, 7);
      if (sprj_fade_ptr_) {
        LogWrite("game_state: SprjFade resolved via SoulSplitter AOB");
      }
    } else {
      LogWrite("game_state: SprjFade resolved via TGA AOB");
    }
  }

  char buf[384];
  snprintf(buf, sizeof(buf),
           "game_state: WorldChr*=0x%llX GameDataMan*=0x%llX GameMan*=0x%llX NewMenu*=0x%llX "
           "MenuMan*=0x%llX Loading*=0x%llX FieldArea*=0x%llX SprjFade*=0x%llX",
           static_cast<unsigned long long>(world_chr_ptr_),
           static_cast<unsigned long long>(game_data_man_ptr_),
           static_cast<unsigned long long>(game_man_ptr_),
           static_cast<unsigned long long>(new_menu_system_ptr_),
           static_cast<unsigned long long>(menu_man_ptr_),
           static_cast<unsigned long long>(loading_ptr_),
           static_cast<unsigned long long>(field_area_ptr_),
           static_cast<unsigned long long>(sprj_fade_ptr_));
  LogWrite(buf);

  if (!world_chr_ptr_ || !game_data_man_ptr_) {
    LogWrite("game_state: required patterns missing (WorldChrMan / GameDataMan)");
    return false;
  }
  if (!game_man_ptr_) {
    LogWrite("game_state: GameMan AOB not found (using MenuMan / menu-state title fallback)");
  }
  if (!menu_man_ptr_) {
    LogWrite("game_state: MenuMan pattern not found (MenuMan probes disabled)");
  }
  if (!sprj_fade_ptr_) {
    LogWrite("game_state: SprjFade pattern not found (area title wipe uses load/world-block fallbacks)");
  }
  return true;
}

bool GameState::Init() {
  ready_ = ResolveBases();
  if (ready_) {
    if (!g_event_flags.Init()) {
      LogWrite("game_state: event flags unavailable (boss cheer/applause disabled)");
    }
  }
  return ready_;
}

void GameState::Shutdown() {
  g_event_flags.Shutdown();
  ready_ = false;
  world_chr_ptr_ = 0;
  game_data_man_ptr_ = 0;
  game_man_ptr_ = 0;
  new_menu_system_ptr_ = 0;
  menu_man_ptr_ = 0;
  loading_ptr_ = 0;
  field_area_ptr_ = 0;
}

GameSnapshot GameState::Read() {
  GameSnapshot s{};
  if (!ready_) {
    return s;
  }

  if (loading_ptr_ && IsReadable(loading_ptr_)) {
    s.is_loading = ReadT<std::uint8_t>(loading_ptr_) != 0;
  }

  const auto player_ins = ReadPlayerIns(world_chr_ptr_);
  s.player_valid = player_ins != 0;

  if (player_ins) {
    const auto chr_modules = ReadT<std::uintptr_t>(player_ins + 0x1F90);
    if (chr_modules && IsReadable(chr_modules + 0x18)) {
      const auto chr_data = ReadT<std::uintptr_t>(chr_modules + 0x18);
      if (chr_data) {
        s.player_hp = ReadT<std::int32_t>(chr_data + 0xD8);
        s.player_max_hp = ReadT<std::int32_t>(chr_data + 0xE0);
        s.player_valid = s.player_max_hp > 0 && s.player_max_hp <= 99999 && s.player_hp >= 0 &&
                         s.player_hp <= 99999;

        const auto anim_mid = ReadT<std::uintptr_t>(chr_modules + 0x80);
        if (anim_mid && IsReadable(anim_mid + 0xC8)) {
          s.current_anim = ReadT<std::int32_t>(anim_mid + 0xC8, -1);
          s.anim_valid = true;
        }

        const auto stay_mid = ReadT<std::uintptr_t>(chr_modules + 0x58);
        if (stay_mid && IsReadable(stay_mid + 0x20)) {
          const auto stay_b = ReadT<std::uintptr_t>(stay_mid + 0x20);
          if (stay_b) {
            if (IsReadable(stay_b + 0x2A0)) {
              s.stay_anim_upper = ReadT<std::int32_t>(stay_b + 0x2A0, -1);
              s.anim_valid = true;
            }
            if (IsReadable(stay_b + 0x498)) {
              s.stay_anim_lower = ReadT<std::int32_t>(stay_b + 0x498, -1);
              s.anim_valid = true;
            }
          }
        }
      }
    }

    // SoulsGym PlayerAnimation: WorldChrMan→player→+0x1F90→+0x28→+0x898.
    // Buffer is UTF-16LE (~40 code units); convert to ASCII for matching.
    const auto name_mods = ReadT<std::uintptr_t>(player_ins + 0x1F90);
    if (name_mods && IsReadable(name_mods + 0x28)) {
      const auto name_mid = ReadT<std::uintptr_t>(name_mods + 0x28);
      constexpr std::size_t kMaxUnits = 40;
      if (name_mid && IsReadable(name_mid + 0x898 + kMaxUnits * 2)) {
        const auto* src = reinterpret_cast<const char16_t*>(name_mid + 0x898);
        char buf[sizeof(s.anim_name)]{};
        std::size_t o = 0;
        for (std::size_t i = 0; i < kMaxUnits && o + 1 < sizeof(buf); ++i) {
          const char16_t u = src[i];
          if (u == 0) {
            break;
          }
          if (u < 0x20 || u > 0x7E) {
            buf[0] = '\0';
            o = 0;
            break;
          }
          buf[o++] = static_cast<char>(u);
        }
        if (o > 0) {
          std::memcpy(s.anim_name, buf, sizeof(s.anim_name));
          s.anim_name_valid = true;
        }
      }
    }
  }

  if (game_data_man_ptr_ && IsReadable(game_data_man_ptr_)) {
    const auto game_data = ReadT<std::uintptr_t>(game_data_man_ptr_);
    if (game_data) {
      s.deaths = ReadT<std::int32_t>(game_data + 0x98);
    }
    ReadEstusCharges(s, game_data_man_ptr_);
  }

  if (new_menu_system_ptr_ && IsReadable(new_menu_system_ptr_)) {
    const auto menu = ReadT<std::uintptr_t>(new_menu_system_ptr_);
    if (menu && IsReadable(menu + 0x88)) {
      s.menu_state = ReadT<std::int32_t>(menu + 0x88, -1);
    }
  }

  bool on_title = false;
  std::uintptr_t menu_man = 0;
  if (menu_man_ptr_ && IsReadable(menu_man_ptr_)) {
    menu_man = ReadT<std::uintptr_t>(menu_man_ptr_);
  }

  if (game_man_ptr_ && IsReadable(game_man_ptr_)) {
    const auto game_man = ReadT<std::uintptr_t>(game_man_ptr_);
    if (game_man && IsReadable(game_man + 0xB1D)) {
      s.game_man_title = ReadT<std::uint8_t>(game_man + 0xB1D);
      if (s.game_man_title != 0 && !s.player_valid) {
        on_title = true;
      }
    }
  }

  if (!on_title && menu_man && IsReadable(menu_man + 0x80)) {
    if (ReadT<std::int32_t>(menu_man + 0x80) == 1 && !s.player_valid) {
      on_title = true;
    }
  }

  if (!on_title && !s.player_valid && !s.is_loading && s.menu_state == 2) {
    on_title = true;
  }

  s.on_title_screen = on_title;

  if (menu_man) {
    s.menu_man_probe_valid = true;
    for (int i = 0; i < kMenuManProbeFlagCount; ++i) {
      const auto off = static_cast<std::uintptr_t>(kMenuManFlagsOffset + i * 4);
      if (IsReadable(menu_man + off)) {
        s.menu_man_flags_probe[i] = ReadT<std::int32_t>(menu_man + off, 0);
      }
    }
    if (IsReadable(menu_man + 0x100)) {
      s.menu_man_i32_100 = ReadT<std::int32_t>(menu_man + 0x100, 0);
    }
    if (IsReadable(menu_man + 0x104)) {
      s.menu_man_i32_104 = ReadT<std::int32_t>(menu_man + 0x104, 0);
    }
    if (IsReadable(menu_man + 0x108)) {
      s.menu_man_i32_108 = ReadT<std::int32_t>(menu_man + 0x108, 0);
    }

    s.banner_flag_idx = -1;
    s.banner_message = 0;
    for (int i = 0; i < kMenuManFlagCount; ++i) {
      const auto off = static_cast<std::uintptr_t>(kMenuManFlagsOffset + i * 4);
      if (!IsReadable(menu_man + off)) {
        continue;
      }
      const auto flag = ReadT<std::int32_t>(menu_man + off, 0);
      if (flag == static_cast<std::int32_t>(kBannerCurrentLocation)) {
        s.banner_message = kBannerCurrentLocation;
        s.banner_flag_idx = i;
        s.banner_valid = true;
        break;
      }
    }
    if (!s.banner_valid && IsReadable(menu_man + 0x104)) {
      const auto legacy = ReadT<std::uint8_t>(menu_man + 0x104);
      if (legacy == kBannerCurrentLocation) {
        s.banner_message = legacy;
        s.banner_valid = true;
      }
    }
  }

  if (sprj_fade_ptr_) {
    float title_remain = 0.f;
    float title_alpha = 0.f;
    float mapin_remain = 0.f;
    float mapin_alpha = 0.f;
    if (ReadSprjFadeTitleFade(sprj_fade_ptr_, &title_remain, &title_alpha, &mapin_remain,
                              &mapin_alpha)) {
      s.title_fade_remain = title_remain;
      s.title_fade_alpha = title_alpha;
      s.mapin_fade_remain = mapin_remain;
      s.mapin_fade_alpha = mapin_alpha;
      s.title_fade_valid = true;
      static bool logged = false;
      if (!logged) {
        logged = true;
        LogWrite("game_state: SprjFade panel readable (area title fade tracking live)");
      }
      if (title_remain > 0.05f || mapin_remain > 0.05f) {
        static float last_log_score = 0.f;
        const float score = std::max(title_remain, mapin_remain);
        if (score - last_log_score > 0.25f) {
          last_log_score = score;
          LogWrite("game_state: SprjFade live title=" + std::to_string(title_remain) + "/" +
                   std::to_string(title_alpha) + " mapin=" + std::to_string(mapin_remain) + "/" +
                   std::to_string(mapin_alpha));
        }
      }
    }
  }

  if (field_area_ptr_ && IsReadable(field_area_ptr_)) {
    const auto field_area = ReadT<std::uintptr_t>(field_area_ptr_);
    if (field_area && IsReadable(field_area + 0x20)) {
      s.world_block_id = ReadT<std::int32_t>(field_area + 0x20);
      s.world_block_valid = true;
    }
    if (field_area && IsReadable(field_area + 0x10)) {
      const auto world_info = ReadT<std::uintptr_t>(field_area + 0x10);
      if (world_info && IsReadable(world_info + 0x10)) {
        const int len = ReadT<std::int32_t>(world_info + 0x8, 0);
        const auto list = ReadT<std::uintptr_t>(world_info + 0x10);
        if (len > 0 && list && IsReadable(list + 0xB)) {
          s.area_number = ReadT<std::uint8_t>(list + 0xB);
          s.area_valid = true;
        }
      }
    }
  }

  s.in_gameplay = s.player_valid && !s.is_loading && !s.on_title_screen;

  if (g_event_flags.Ready() && s.in_gameplay) {
    std::unordered_set<std::int32_t> cheer_seen;
    std::unordered_set<std::int32_t> defeat_seen;
    for (const auto& pair : kBossPairs) {
      bool cheer = false;
      bool defeated = false;
      if (!g_event_flags.ReadFlag(pair.cheer_flag, &cheer)) {
        continue;
      }
      if (!g_event_flags.ReadFlag(pair.defeat_flag, &defeated)) {
        continue;
      }
      if (cheer && cheer_seen.insert(pair.cheer_flag).second) {
        s.cheer_flags_on.push_back(pair.cheer_flag);
      }
      if (defeated && defeat_seen.insert(pair.defeat_flag).second) {
        s.defeat_flags_on.push_back(pair.defeat_flag);
      }
      if (cheer && !defeated && !s.boss_fight_active) {
        s.boss_fight_active = true;
        s.boss_defeat_flag = pair.defeat_flag;
        s.boss_cheer_flag = pair.cheer_flag;
      }
    }
  }

  return s;
}

}  // namespace sitcom
