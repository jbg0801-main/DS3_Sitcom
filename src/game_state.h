#pragma once

#include <cstdint>
#include <vector>

namespace sitcom {

constexpr int kMenuManFlagsOffset = 0x32;
constexpr int kMenuManFlagCount = 500;
constexpr int kMenuManProbeFlagCount = 25;
constexpr std::uint8_t kBannerCurrentLocation = 8;  // EMEVD Display Banner — area title card

struct BossPair {
  std::int32_t cheer_flag;
  std::int32_t defeat_flag;
};

struct GameSnapshot {
  bool player_valid = false;
  bool in_gameplay = false;
  bool is_loading = false;

  std::int32_t player_hp = 0;
  std::int32_t player_max_hp = 0;
  std::int32_t deaths = 0;

  bool boss_fight_active = false;
  std::int32_t boss_cheer_flag = 0;
  std::int32_t boss_defeat_flag = 0;
  std::vector<std::int32_t> cheer_flags_on;
  std::vector<std::int32_t> defeat_flags_on;

  bool area_valid = false;
  std::uint8_t area_number = 255;
  bool world_block_valid = false;
  std::int32_t world_block_id = -1;
  bool title_fade_valid = false;
  float title_fade_remain = 0.f;
  float title_fade_alpha = 0.f;
  float mapin_fade_remain = 0.f;
  float mapin_fade_alpha = 0.f;
  bool banner_valid = false;
  std::uint8_t banner_message = 0;
  std::int32_t banner_flag_idx = -1;
  bool menu_man_probe_valid = false;
  std::int32_t menu_man_flags_probe[kMenuManProbeFlagCount]{};
  std::int32_t menu_man_i32_100 = 0;
  std::int32_t menu_man_i32_104 = 0;
  std::int32_t menu_man_i32_108 = 0;

  bool on_title_screen = false;
  std::uint8_t game_man_title = 0;
  std::int32_t menu_state = -1;

  std::int32_t current_anim = -1;
  std::int32_t stay_anim_upper = -1;
  std::int32_t stay_anim_lower = -1;
  bool anim_valid = false;

  // SoulsGym PlayerAnimation string (WorldChrMan→+0x80→+0x1F90→+0x28→+0x898).
  char anim_name[48]{};
  bool anim_name_valid = false;

  // Sum of HP + ashen estus flask quantities in player inventory (-1 = unread).
  std::int32_t estus_charges = -1;
  bool estus_charges_valid = false;
};

class GameState {
 public:
  bool Init();
  void Shutdown();
  GameSnapshot Read();

 private:
  bool ResolveBases();

  bool ready_ = false;
  std::uintptr_t world_chr_ptr_ = 0;
  std::uintptr_t game_data_man_ptr_ = 0;
  std::uintptr_t game_man_ptr_ = 0;
  std::uintptr_t new_menu_system_ptr_ = 0;
  std::uintptr_t menu_man_ptr_ = 0;
  std::uintptr_t loading_ptr_ = 0;
  std::uintptr_t field_area_ptr_ = 0;
  std::uintptr_t sprj_fade_ptr_ = 0;
};

}  // namespace sitcom
