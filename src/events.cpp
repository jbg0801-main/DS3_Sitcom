#include "events.h"

#include "fmod_volume.h"
#include "game_state.h"
#include "item_hooks.h"
#include "log.h"

#include <windows.h>

#include <cmath>
#include <string>
#include <unordered_set>

namespace sitcom {
namespace {

// DS3 TAE ids — confirm with log=true + probe lines if a reaction misfires.
// Placeholders follow DSR ids until playtested on DS3 v1.15.x.

bool IsPickupAnim(std::int32_t anim) {
  return anim == 7520 || anim == 7522;
}

bool IsLadderFallAnim(std::int32_t anim) {
  return anim == 1560 || (anim >= 7050 && anim <= 7052);
}

bool IsFailCastAnim(std::int32_t anim) {
  return anim == 6299 || anim == 6399;
}

// DS3 TAE behavior ids (anim may appear as 19001 or 10019001).
std::int32_t AnimBehavior(std::int32_t anim) {
  if (anim < 0) {
    return -1;
  }
  return anim % 100000;
}

// ESD 017 Estus Empty — playtest: 10019001→10019002 loop. Not 50113 (2H) or 290xx (block).
bool IsEmptyEstusStartAnim(std::int32_t anim) {
  return AnimBehavior(anim) == 19001;
}

bool IsEstusProbeAnim(std::int32_t anim) {
  const int b = AnimBehavior(anim);
  return b == 19001 || b == 19002 || (b >= 50110 && b <= 50112);
}

bool IsBlockOrTwoHandAnim(std::int32_t anim) {
  const int b = AnimBehavior(anim);
  return b == 29040 || b == 29012 || b == 29025 || b == 50113 || b == 50114;
}

// Weapon swap / block anims that briefly pass through 19001 without an empty flask.
bool IsWeaponBlockOrSwapAnim(std::int32_t anim) {
  if (IsBlockOrTwoHandAnim(anim)) {
    return true;
  }
  const int b = AnimBehavior(anim);
  if (b >= 29000 && b <= 29100) {
    return true;
  }
  if (b >= 27100 && b <= 27200) {
    return true;
  }
  if (b == 23100 || b == 68010 || b == 68011 || b == 68012) {
    return true;
  }
  return false;
}

bool PrevAnimBlocksEmptyEstus(const GameSnapshot& prev) {
  return IsWeaponBlockOrSwapAnim(prev.current_anim) ||
         IsWeaponBlockOrSwapAnim(prev.stay_anim_upper) ||
         IsWeaponBlockOrSwapAnim(prev.stay_anim_lower);
}

bool IsEstusEmptyContext(const GameSnapshot& snap, const GameSnapshot& prev,
                         const GameSnapshot* prev2) {
  if (AnimBehavior(snap.current_anim) != 19001) {
    return false;
  }
  if (PrevAnimBlocksEmptyEstus(prev)) {
    return false;
  }
  const int pb = AnimBehavior(prev.current_anim);
  if (prev2 && prev2->anim_valid) {
    const int ppb = AnimBehavior(prev2->current_anim);
    if ((pb == 20210 || pb == 20110 || pb == 20010) &&
        (ppb >= 27100 && ppb <= 27200 || ppb >= 29000 && ppb <= 29100)) {
      return false;
    }
  }
  if (pb == 0 || pb == 19000 || pb == 19100 || pb == 19002 || pb == 20210 || pb == 20110 ||
      pb == 20010 || pb == 50110 || pb == 50111 || pb == 50112) {
    return true;
  }
  return false;
}

void LogMenuManProbe(const char* tag, const GameSnapshot& snap) {
  if (!snap.menu_man_probe_valid) {
    return;
  }
  std::string msg = std::string("probe: ") + tag + " area=" + std::to_string(snap.area_number) +
                    " block=" + std::to_string(snap.world_block_id) +
                    " banner_idx=" + std::to_string(snap.banner_flag_idx) + " flags[0..24]=";
  for (int i = 0; i < kMenuManProbeFlagCount; ++i) {
    if (i > 0) {
      msg += ',';
    }
    msg += std::to_string(snap.menu_man_flags_probe[i]);
  }
  msg += " i32@100=" + std::to_string(snap.menu_man_i32_100) +
         " i32@104=" + std::to_string(snap.menu_man_i32_104) +
         " i32@108=" + std::to_string(snap.menu_man_i32_108);
  LogWrite(msg);
}

bool AnimChanged(const GameSnapshot& snap, const GameSnapshot& prev) {
  return snap.current_anim != prev.current_anim || snap.stay_anim_upper != prev.stay_anim_upper ||
         snap.stay_anim_lower != prev.stay_anim_lower;
}

bool AnimEntered(const GameSnapshot& snap, const GameSnapshot& prev, bool (*pred)(std::int32_t)) {
  return snap.anim_valid && prev.anim_valid && pred(snap.current_anim) && !pred(prev.current_anim);
}

bool AnyAnimEntered(const GameSnapshot& snap, const GameSnapshot& prev, bool (*pred)(std::int32_t)) {
  if (!snap.anim_valid || !prev.anim_valid) {
    return false;
  }
  const bool now = pred(snap.current_anim) || pred(snap.stay_anim_upper) || pred(snap.stay_anim_lower);
  const bool was = pred(prev.current_anim) || pred(prev.stay_anim_upper) || pred(prev.stay_anim_lower);
  return now && !was;
}

bool TryFailLaugh(std::uint64_t now, std::uint64_t* last_ms, float cooldown_sec, Audio& audio,
                  const char* label) {
  const auto cd = static_cast<std::uint64_t>(cooldown_sec * 1000.0f);
  if (now - *last_ms < cd) {
    return false;
  }
  LogWrite(std::string("event: fail laugh — ") + label);
  audio.Play(SoundCategory::Laugh);
  *last_ms = now;
  return true;
}

}  // namespace

void EventDetector::Reset() {
  has_prev_ = false;
  has_prev2_ = false;
  prev_ = {};
  prev2_ = {};
  last_laugh_hit_ms_ = 0;
  last_ooh_ms_ = 0;
  last_fail_laugh_ms_ = 0;
  pending_hit_laugh_ms_ = 0;
  last_wipe_ms_ = 0;
  last_anim_probe_ms_ = 0;
  loading_from_gameplay_ = false;
  spawn_load_seen_ = false;
  block_baseline_seen_ = false;
  applause_armed_ = false;
  armed_defeat_flag_ = 0;
  flags_seeded_ = false;
  gameplay_seeded_ms_ = 0;
}

void EventDetector::Update(const GameSnapshot& snap, const Config& cfg, Audio& audio) {
  if (!cfg.enabled) {
    return;
  }

  const auto now = static_cast<std::uint64_t>(GetTickCount64());
  constexpr int kGoodsCategory = 0x40000000;

  if (pending_hit_laugh_ms_ != 0 && now >= pending_hit_laugh_ms_) {
    pending_hit_laugh_ms_ = 0;
    if (cfg.laugh_on_hit && snap.in_gameplay) {
      const auto cooldown_ms = static_cast<std::uint64_t>(cfg.laugh_hit_seconds * 1000.0f);
      if (now - last_laugh_hit_ms_ >= cooldown_ms) {
        LogWrite("event: player hit (deferred)");
        audio.Play(SoundCategory::Laugh);
        last_laugh_hit_ms_ = now;
      }
    }
  }

  const int got = ItemHooksConsumeAcquired();
  const int suppressed = ItemHooksConsumeSuppressed();
  const int item_id = ItemHooksLastItemId();
  const int item_cat = ItemHooksLastCategory();
  bool ooh_from_itemget = false;
  if (cfg.ooh_on_item_get && snap.in_gameplay && !snap.on_title_screen && flags_seeded_ &&
      gameplay_seeded_ms_ != 0 && (now - gameplay_seeded_ms_) >= 2500 && got == 1 &&
      suppressed == 0 &&
      (item_cat == kGoodsCategory || (item_id > 0 && item_id < 10000))) {
    ooh_from_itemget = true;
  } else if (got > 0 && got <= 2) {
    char buf[96];
    snprintf(buf, sizeof(buf), "event: ItemGet skip (x%d cat=0x%X id=%d)", got,
             static_cast<unsigned>(item_cat), item_id);
    LogWrite(buf);
  }

  if (!has_prev_) {
    prev_ = snap;
    has_prev_ = true;
    if (snap.in_gameplay) {
      flags_seeded_ = true;
      gameplay_seeded_ms_ = now;
    }
    return;
  }

  const auto advance_history = [&]() {
    prev2_ = prev_;
    has_prev2_ = has_prev_;
    prev_ = snap;
    has_prev_ = true;
  };

  if (cfg.ooh_on_item_get && snap.in_gameplay && !snap.on_title_screen) {
    const bool from_anim = AnimEntered(snap, prev_, IsPickupAnim);
    if (from_anim || ooh_from_itemget) {
      const auto cd = static_cast<std::uint64_t>(cfg.ooh_cooldown_seconds * 1000.0f);
      if (now - last_ooh_ms_ >= cd) {
        if (from_anim) {
          LogWrite("event: item pickup (anim=" + std::to_string(snap.current_anim) + ")");
        } else {
          LogWrite("event: item pickup (ItemGet goods id=" + std::to_string(item_id) + ")");
        }
        audio.Play(SoundCategory::Ooh);
        last_ooh_ms_ = now;
      }
    }
  }

  const bool wipe_ready = cfg.wipe_on_area_title && flags_seeded_ && spawn_load_seen_ &&
                          snap.in_gameplay && !snap.is_loading &&
                          now - last_wipe_ms_ >= kWipeCooldownMs;

  // Area title popup — MenuMan banner fields (DS3 may use i32@104, not DSR byte-only).
  if (wipe_ready && snap.menu_man_probe_valid && prev_.menu_man_probe_valid &&
      snap.menu_man_i32_104 == static_cast<std::int32_t>(kBannerCurrentLocation) &&
      prev_.menu_man_i32_104 != static_cast<std::int32_t>(kBannerCurrentLocation)) {
    LogWrite("event: area title card (MenuMan i32@104=" +
             std::to_string(snap.menu_man_i32_104) + ")");
    audio.Play(SoundCategory::SceneWipe);
    last_wipe_ms_ = now;
  }

  // Area title popup — MenuMan flags[] value 8 (Current Location).
  if (wipe_ready && snap.banner_valid && prev_.banner_valid &&
      snap.banner_message == kBannerCurrentLocation &&
      prev_.banner_message != kBannerCurrentLocation) {
    LogWrite("event: area title card (MenuMan banner=" +
             std::to_string(static_cast<unsigned>(snap.banner_message)) +
             (snap.banner_flag_idx >= 0 ? " idx=" + std::to_string(snap.banner_flag_idx) : "") +
             ")");
    audio.Play(SoundCategory::SceneWipe);
    last_wipe_ms_ = now;
  }

  // Area title popup — SprjFade Title / MapIn (fires at map trigger volumes, not on a load timer).
  if (wipe_ready && snap.title_fade_valid && prev_.title_fade_valid) {
    const auto fade_started = [](float cur_remain, float prev_remain, float cur_alpha,
                                 float prev_alpha) {
      return (cur_remain > 0.05f && prev_remain <= 0.05f) ||
             (cur_alpha >= 0.05f && prev_alpha < 0.05f);
    };
    const bool title_started =
        fade_started(snap.title_fade_remain, prev_.title_fade_remain, snap.title_fade_alpha,
                     prev_.title_fade_alpha) ||
        fade_started(snap.mapin_fade_remain, prev_.mapin_fade_remain, snap.mapin_fade_alpha,
                     prev_.mapin_fade_alpha);
    if (title_started) {
      LogWrite("event: area title card (SprjFade title=" +
               std::to_string(snap.title_fade_remain) + "/" + std::to_string(snap.title_fade_alpha) +
               " mapin=" + std::to_string(snap.mapin_fade_remain) + "/" +
               std::to_string(snap.mapin_fade_alpha) + ")");
      audio.Play(SoundCategory::SceneWipe);
      last_wipe_ms_ = now;
    }
  }

  // Area transition sting — same moment as the vanilla location SFX (FMOD_Event_Start hook).
  if (wipe_ready) {
    char fmod_name[128]{};
    if (FmodConsumeAreaTransition(fmod_name, static_cast<int>(sizeof(fmod_name)))) {
      LogWrite(std::string("event: area title card (FMOD ") + fmod_name + ")");
      audio.Play(SoundCategory::SceneWipe);
      last_wipe_ms_ = now;
    }
  }

  // Seamless walk only: block id changes while already in-world (Fog gate / door boundaries).
  // Bonfire warps change block at load-end; the title card comes later at a map trigger — SprjFade/FMOD.
  if (wipe_ready && snap.world_block_valid && prev_.world_block_valid && !prev_.is_loading &&
      snap.world_block_id != prev_.world_block_id && prev_.world_block_id >= 0 &&
      snap.world_block_id >= 0 && block_baseline_seen_) {
    LogWrite("event: area title card (world_block walk " + std::to_string(prev_.world_block_id) +
             "→" + std::to_string(snap.world_block_id) + ")");
    audio.Play(SoundCategory::SceneWipe);
    last_wipe_ms_ = now;
  }

  // Adopt block baseline after load without wiping (title card is trigger-based, not load-timed).
  if (snap.in_gameplay && snap.world_block_valid && snap.world_block_id >= 0) {
    if (prev_.is_loading && !snap.is_loading) {
      block_baseline_seen_ = true;
    } else if (!block_baseline_seen_) {
      block_baseline_seen_ = true;
    }
  }

  // Track spawn load (no wipe on load end).
  if (prev_.in_gameplay && !prev_.is_loading && snap.is_loading) {
    loading_from_gameplay_ = true;
  }
  if (loading_from_gameplay_ && prev_.is_loading && !snap.is_loading && !snap.in_gameplay) {
    loading_from_gameplay_ = false;
  }
  if (loading_from_gameplay_ && prev_.is_loading && !snap.is_loading && snap.in_gameplay) {
    loading_from_gameplay_ = false;
    if (!spawn_load_seen_) {
      spawn_load_seen_ = true;
    }
  } else if (prev_.is_loading && !snap.is_loading && snap.in_gameplay && !spawn_load_seen_) {
    spawn_load_seen_ = true;
  }

  // Legacy area id — seamless walk only (same rule as world_block; not on load-end).
  if (wipe_ready && snap.area_valid && prev_.area_valid && !prev_.is_loading &&
      snap.area_number != prev_.area_number && prev_.area_number != 255 &&
      snap.area_number != 255) {
    LogWrite("event: area title card (area walk " + std::to_string(prev_.area_number) + "→" +
             std::to_string(snap.area_number) + ")");
    audio.Play(SoundCategory::SceneWipe);
    last_wipe_ms_ = now;
  }

  if (cfg.log && cfg.probe_transitions && snap.menu_man_probe_valid) {
    if (!prev_.is_loading && snap.is_loading) {
      LogMenuManProbe("loading-start", snap);
    }
    if (prev_.is_loading && !snap.is_loading) {
      LogMenuManProbe("loading-end", snap);
    }
    if (snap.world_block_valid && prev_.world_block_valid &&
        snap.world_block_id != prev_.world_block_id) {
      LogMenuManProbe("world-block-change", snap);
    }
    if (snap.menu_man_i32_104 != prev_.menu_man_i32_104) {
      LogWrite("probe: menu-i32@104 " + std::to_string(prev_.menu_man_i32_104) + "→" +
               std::to_string(snap.menu_man_i32_104));
    }
    if (snap.title_fade_valid && prev_.title_fade_valid &&
        (std::fabs(snap.title_fade_remain - prev_.title_fade_remain) > 0.05f ||
         std::fabs(snap.title_fade_alpha - prev_.title_fade_alpha) > 0.05f ||
         std::fabs(snap.mapin_fade_remain - prev_.mapin_fade_remain) > 0.05f ||
         std::fabs(snap.mapin_fade_alpha - prev_.mapin_fade_alpha) > 0.05f)) {
      LogWrite("probe: title-fade title=" + std::to_string(prev_.title_fade_remain) + "→" +
               std::to_string(snap.title_fade_remain) + "/" +
               std::to_string(prev_.title_fade_alpha) + "→" +
               std::to_string(snap.title_fade_alpha) + " mapin=" +
               std::to_string(prev_.mapin_fade_remain) + "→" +
               std::to_string(snap.mapin_fade_remain) + "/" +
               std::to_string(prev_.mapin_fade_alpha) + "→" +
               std::to_string(snap.mapin_fade_alpha));
    }
    if (cfg.probe_transitions && wipe_ready &&
        snap.title_fade_valid &&
        (snap.title_fade_remain > 0.05f || snap.mapin_fade_remain > 0.05f ||
         snap.title_fade_alpha > 0.05f || snap.mapin_fade_alpha > 0.05f)) {
      static std::uint64_t last_live_ms = 0;
      if (now - last_live_ms >= 500) {
        last_live_ms = now;
        LogWrite("probe: title-fade live title=" + std::to_string(snap.title_fade_remain) + "/" +
                 std::to_string(snap.title_fade_alpha) + " mapin=" +
                 std::to_string(snap.mapin_fade_remain) + "/" +
                 std::to_string(snap.mapin_fade_alpha));
      }
    }
  }

  if (!snap.in_gameplay) {
    if (prev_.in_gameplay) {
      flags_seeded_ = false;
      gameplay_seeded_ms_ = 0;
      applause_armed_ = false;
      armed_defeat_flag_ = 0;
      pending_hit_laugh_ms_ = 0;
    }
    advance_history();
    return;
  }

  if (!flags_seeded_) {
    flags_seeded_ = true;
    gameplay_seeded_ms_ = now;
    advance_history();
    LogWrite("event: seeded flag baselines in gameplay");
    return;
  }

  if (snap.player_valid && prev_.player_valid && prev_.in_gameplay) {
    const bool max_hp_changed = snap.player_max_hp != prev_.player_max_hp;
    if (!max_hp_changed && snap.player_hp < prev_.player_hp) {
      if (snap.player_hp <= 0) {
        if (cfg.laugh_on_death) {
          LogWrite("event: player death");
          audio.Play(SoundCategory::Laugh);
        }
      } else if (cfg.laugh_on_hit) {
        const auto cooldown_ms = static_cast<std::uint64_t>(cfg.laugh_hit_seconds * 1000.0f);
        if (now - last_laugh_hit_ms_ >= cooldown_ms && pending_hit_laugh_ms_ == 0) {
          pending_hit_laugh_ms_ = now + kHitLaughDeferMs;
        }
      }
    } else if (snap.deaths > prev_.deaths) {
      if (cfg.laugh_on_death) {
        LogWrite("event: death counter increased");
        audio.Play(SoundCategory::Laugh);
      }
    }
  }

  if (cfg.cheer_on_boss_bar && snap.boss_fight_active && snap.boss_cheer_flag != 0) {
    std::unordered_set<std::int32_t> prev_cheer(prev_.cheer_flags_on.begin(),
                                                prev_.cheer_flags_on.end());
    if (prev_cheer.count(snap.boss_cheer_flag) == 0) {
      LogWrite("event: boss fight start cheer_flag=" + std::to_string(snap.boss_cheer_flag) +
               " defeat_flag=" + std::to_string(snap.boss_defeat_flag));
      audio.Play(SoundCategory::Cheer);
      applause_armed_ = true;
      armed_defeat_flag_ = snap.boss_defeat_flag;
    }
  }

  if (cfg.applause_on_boss_death) {
    std::unordered_set<std::int32_t> prev_def(prev_.defeat_flags_on.begin(),
                                              prev_.defeat_flags_on.end());
    for (const auto id : snap.defeat_flags_on) {
      if (prev_def.count(id) != 0) {
        continue;
      }
      LogWrite("event: boss defeated flag=" + std::to_string(id));
      audio.Play(SoundCategory::Applause);
      if (armed_defeat_flag_ == id) {
        applause_armed_ = false;
        armed_defeat_flag_ = 0;
      }
    }
  }

  if (snap.anim_valid && prev_.anim_valid) {
    if (cfg.laugh_on_empty_flask && AnimChanged(snap, prev_) &&
        (IsEstusProbeAnim(snap.current_anim) || IsEstusProbeAnim(snap.stay_anim_upper) ||
         IsEstusProbeAnim(snap.stay_anim_lower)) &&
        now - last_anim_probe_ms_ >= kAnimProbeMinMs) {
      LogWrite("probe: estus-anim cur=" + std::to_string(prev_.current_anim) + "→" +
               std::to_string(snap.current_anim) + " stay=" + std::to_string(prev_.stay_anim_upper) +
               "/" + std::to_string(prev_.stay_anim_lower) + "→" +
               std::to_string(snap.stay_anim_upper) + "/" + std::to_string(snap.stay_anim_lower));
      last_anim_probe_ms_ = now;
    } else if (cfg.log && AnimChanged(snap, prev_) && now - last_anim_probe_ms_ >= kAnimProbeMinMs) {
      LogWrite("probe: anim-change cur=" + std::to_string(prev_.current_anim) + "→" +
               std::to_string(snap.current_anim) + " stay=" + std::to_string(prev_.stay_anim_upper) +
               "/" + std::to_string(prev_.stay_anim_lower) + "→" +
               std::to_string(snap.stay_anim_upper) + "/" + std::to_string(snap.stay_anim_lower));
      last_anim_probe_ms_ = now;
    }

    if (cfg.laugh_on_empty_flask && AnimEntered(snap, prev_, IsEmptyEstusStartAnim) &&
        IsEstusEmptyContext(snap, prev_, has_prev2_ ? &prev2_ : nullptr)) {
      const std::string label = "empty flask (anim=" + std::to_string(snap.current_anim) +
                                " estus=" + std::to_string(snap.estus_charges) + ")";
      TryFailLaugh(now, &last_fail_laugh_ms_, cfg.laugh_hit_seconds, audio, label.c_str());
    }
    if (cfg.laugh_on_locked_door && AnimEntered(snap, prev_, [](std::int32_t a) {
          return a == 7510;
        })) {
      TryFailLaugh(now, &last_fail_laugh_ms_, cfg.laugh_hit_seconds, audio,
                   "locked/invalid use (anim=7510)");
    }
    if (cfg.laugh_on_no_spell && AnimEntered(snap, prev_, IsFailCastAnim)) {
      TryFailLaugh(now, &last_fail_laugh_ms_, cfg.laugh_hit_seconds, audio,
                   "no spell (fail-cast anim)");
    }
    if (cfg.laugh_on_ladder_fall && AnimEntered(snap, prev_, IsLadderFallAnim)) {
      const std::string label = "ladder fall (anim=" + std::to_string(snap.current_anim) + ")";
      TryFailLaugh(now, &last_fail_laugh_ms_, cfg.laugh_hit_seconds, audio, label.c_str());
    }
  }

  advance_history();
}

}  // namespace sitcom
