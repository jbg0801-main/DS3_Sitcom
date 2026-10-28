#pragma once

#include "audio.h"
#include "config.h"
#include "game_state.h"

#include <cstdint>

namespace sitcom {

class EventDetector {
 public:
  void Reset();
  void Update(const GameSnapshot& snap, const Config& cfg, Audio& audio);

 private:
  bool has_prev_ = false;
  bool has_prev2_ = false;
  GameSnapshot prev_{};
  GameSnapshot prev2_{};
  std::uint64_t last_laugh_hit_ms_ = 0;
  std::uint64_t last_ooh_ms_ = 0;
  std::uint64_t last_fail_laugh_ms_ = 0;
  bool applause_armed_ = false;
  std::int32_t armed_defeat_flag_ = 0;
  bool flags_seeded_ = false;
  std::uint64_t gameplay_seeded_ms_ = 0;
  std::uint64_t pending_hit_laugh_ms_ = 0;
  std::uint64_t last_wipe_ms_ = 0;
  std::uint64_t last_anim_probe_ms_ = 0;
  bool loading_from_gameplay_ = false;
  bool spawn_load_seen_ = false;
  bool block_baseline_seen_ = false;
  std::int32_t last_seen_block_ = -1;
  std::int32_t last_seen_area_ = 255;
  static constexpr std::uint64_t kHitLaughDeferMs = 300;
  static constexpr std::uint64_t kWipeCooldownMs = 10000;
  static constexpr std::uint64_t kAnimProbeMinMs = 200;
};

}  // namespace sitcom
