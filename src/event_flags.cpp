#include "event_flags.h"

#include "log.h"

#include <windows.h>
#include <psapi.h>

#include <cstring>
#include <string>

namespace sitcom {
namespace {

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

}  // namespace

int EventFlags::ResolveWorldBlockCategory(std::int32_t flag_id) const {
  const auto event_flag_area = static_cast<int>((flag_id / 100000) % 100);
  const auto event_flag_id_div_10000 = static_cast<int>((flag_id / 10000) % 10);

  int flag_world_block_info_category = -1;
  if (event_flag_area >= 90 || event_flag_area + event_flag_id_div_10000 == 0) {
    return 0;
  }

  if (!field_area_ptr_ || !IsReadable(field_area_ptr_)) {
    return -1;
  }
  const auto field_area = *reinterpret_cast<std::uintptr_t*>(field_area_ptr_);
  if (!field_area || !IsReadable(field_area + 0x10)) {
    return -1;
  }
  const auto world_info_owner = *reinterpret_cast<std::uintptr_t*>(field_area + 0x10);
  if (!world_info_owner || !IsReadable(world_info_owner + 0x10)) {
    return -1;
  }

  const int size = *reinterpret_cast<const int*>(world_info_owner + 0x8);
  const auto vector_base = world_info_owner + 0x10;

  for (int i = 0; i < size; ++i) {
    const auto entry = vector_base + static_cast<std::uintptr_t>(i) * 0x38;
    if (!IsReadable(entry + 0x28)) {
      continue;
    }
    const auto area = *reinterpret_cast<const std::uint8_t*>(entry + 0xB);
    if (area != static_cast<std::uint8_t>(event_flag_area)) {
      continue;
    }

    const auto count = *reinterpret_cast<const std::uint8_t*>(entry + 0x20);
    int index = 0;
    bool found = false;
    std::uintptr_t world_info_block_vector = 0;

    if (count >= 1) {
      while (true) {
        world_info_block_vector = entry + 0x28;
        if (!IsReadable(world_info_block_vector + static_cast<std::uintptr_t>(index) * 0x70 + 0x8)) {
          break;
        }
        const auto flag = *reinterpret_cast<const int*>(
            world_info_block_vector + static_cast<std::uintptr_t>(index) * 0x70 + 0x8);
        if (((flag >> 0x10) & 0xFF) == event_flag_id_div_10000 && (flag >> 0x18) == event_flag_area) {
          found = true;
          break;
        }
        ++index;
        if (count <= index) {
          found = false;
          break;
        }
      }
    }

    if (found) {
      flag_world_block_info_category =
          *reinterpret_cast<const int*>(world_info_block_vector +
                                        static_cast<std::uintptr_t>(index) * 0x70 + 0x20);
      break;
    }
  }

  if (flag_world_block_info_category >= 0) {
    ++flag_world_block_info_category;
  }
  return flag_world_block_info_category;
}

bool EventFlags::Resolve() {
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

  event_flag_man_slot_ = 0;

  // TGA SprjEventFlagMan / GameFlagData (runtime; preferred over init-store slot)
  const std::uint8_t pat_ef1[] = {0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x44, 0x0F, 0xB6, 0xCB,
                                    0x41, 0xB8, 0x07, 0x00, 0x00, 0x00, 0x8B, 0xD6};
  const char mask_ef1[] = "xxx????xxxxxxxxxx";
  event_flag_man_slot_ = ScanRipSlot(base, size, pat_ef1, mask_ef1, 3, 7);

  if (!event_flag_man_slot_) {
    // SoulMemory SprjEventFlagMan init store (may be zero until game init)
    const std::uint8_t pat_ef2[] = {0x48, 0xC7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x8B, 0x7C,
                                      0x24, 0x38, 0xC7, 0x46, 0x54, 0xFF, 0xFF, 0xFF, 0xFF, 0x48,
                                      0x83, 0xC4, 0x20, 0x5E, 0xC3};
    const char mask_ef2[] = "xxx????xxxxxxxxxxxxxxxxxxxx";
    event_flag_man_slot_ = ScanRipSlot(base, size, pat_ef2, mask_ef2, 3, 11);
  }

  if (!event_flag_man_slot_ || !IsReadable(event_flag_man_slot_)) {
    LogWrite("event_flags: SprjEventFlagMan AOB not found");
    return false;
  }

  // FieldArea for non-global flag areas
  const std::uint8_t pat_fa[] = {0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x48, 0x85, 0xC9, 0x74, 0x26,
                                 0x44, 0x8B};
  const char mask_fa[] = "xxx????xxxxxxx";
  field_area_ptr_ = ScanRipSlot(base, size, pat_fa, mask_fa, 3, 7);

  event_flag_man_ = 0;
  logged_man_ = false;
  return true;
}

bool EventFlags::EnsureMan() const {
  if (!ready_ || !event_flag_man_slot_ || !IsReadable(event_flag_man_slot_)) {
    return false;
  }
  const auto man = *reinterpret_cast<std::uintptr_t*>(event_flag_man_slot_);
  if (!man) {
    return false;
  }
  if (man != event_flag_man_) {
    event_flag_man_ = man;
    if (!logged_man_) {
      char buf[96];
      snprintf(buf, sizeof(buf), "event_flags: man=0x%llX field_area_slot=0x%llX",
               static_cast<unsigned long long>(event_flag_man_),
               static_cast<unsigned long long>(field_area_ptr_));
      LogWrite(buf);
      logged_man_ = true;
    }
  }
  return true;
}

bool EventFlags::Init() {
  ready_ = Resolve();
  return ready_;
}

void EventFlags::Shutdown() {
  ready_ = false;
  event_flag_man_slot_ = 0;
  event_flag_man_ = 0;
  field_area_ptr_ = 0;
  logged_man_ = false;
}

bool EventFlags::ReadFlag(std::int32_t flag_id, bool* out_value) const {
  if (!ready_ || !out_value || flag_id < 0) {
    return false;
  }

  if (!EnsureMan()) {
    return false;
  }

  if (!IsReadable(event_flag_man_ + 0x228) ||
      *reinterpret_cast<const char*>(event_flag_man_ + 0x228) == '\0') {
    return false;
  }

  const auto event_flag_id_div_10000000 = static_cast<int>((flag_id / 10000000) % 10);
  const auto event_flag_id_div_1000 = static_cast<int>((flag_id / 1000) % 10);
  const auto event_flag_id_mod_1000 = static_cast<int>(flag_id % 1000);

  const int flag_world_block_info_category = ResolveWorldBlockCategory(flag_id);
  if (flag_world_block_info_category < 0) {
    return false;
  }

  if (!IsReadable(event_flag_man_ + 0x218)) {
    return false;
  }
  const auto region_ptr = *reinterpret_cast<std::uintptr_t*>(
      event_flag_man_ + 0x218 + static_cast<std::uintptr_t>(event_flag_id_div_10000000) * 0x18);
  if (!region_ptr) {
    return false;
  }

  const auto result_ptr =
      region_ptr + static_cast<std::uintptr_t>(flag_world_block_info_category) * 0xA8 +
      static_cast<std::uintptr_t>(event_flag_id_div_1000) * 0x10;
  if (!IsReadable(result_ptr + static_cast<std::uintptr_t>((event_flag_id_mod_1000 >> 5) * 4))) {
    return false;
  }

  const auto value = *reinterpret_cast<const std::uint32_t*>(
      result_ptr + static_cast<std::uintptr_t>((event_flag_id_mod_1000 >> 5) * 4));
  const auto mask = 1u << (0x1F - (static_cast<unsigned>(event_flag_id_mod_1000) & 0x1F));
  *out_value = (value & mask) != 0;
  return true;
}

}  // namespace sitcom
