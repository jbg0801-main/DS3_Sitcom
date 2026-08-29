#pragma once

#include <cstdint>
#include <vector>

namespace sitcom {

// MenuMan+0x104 "Message" / Display Banner type (EMEVD; same id semantics as DSR).
constexpr std::uint8_t kBannerNone = 0;
constexpr std::uint8_t kBannerCurrentLocation = 8;  // area title card ("Map Name")

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
  bool banner_valid = false;
  std::uint8_t banner_message = 0;

  bool on_title_screen = false;
  std::uint8_t game_man_title = 0;
  std::int32_t menu_state = -1;

  std::int32_t current_anim = -1;
  std::int32_t stay_anim_upper = -1;
  std::int32_t stay_anim_lower = -1;
  bool anim_valid = false;
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
};

}  // namespace sitcom
