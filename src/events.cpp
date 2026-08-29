#include "events.h"

#include "game_state.h"
#include "item_hooks.h"
#include "log.h"

#include <windows.h>

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

bool IsEmptyEstusAnim(std::int32_t anim) {
  return anim == 7588 || anim == 7589;
}

bool IsGoodsProbeAnim(std::int32_t anim) {
  return anim >= 7400 && anim <= 7600;
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
  prev_ = {};
  last_laugh_hit_ms_ = 0;
  last_ooh_ms_ = 0;
  last_fail_laugh_ms_ = 0;
  pending_hit_laugh_ms_ = 0;
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

  // Area title card: MenuMan banner → 8 (Current Location).
  if (cfg.wipe_on_area_title && snap.banner_valid && prev_.banner_valid && snap.area_valid &&
      snap.in_gameplay && !snap.is_loading) {
    if (snap.banner_message == kBannerCurrentLocation &&
        prev_.banner_message != kBannerCurrentLocation) {
      LogWrite("event: area title card (banner=Current_Location)");
      audio.Play(SoundCategory::SceneWipe);
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
    prev_ = snap;
    return;
  }

  if (!flags_seeded_) {
    flags_seeded_ = true;
    gameplay_seeded_ms_ = now;
    prev_ = snap;
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
    if (cfg.laugh_on_empty_flask &&
        (IsGoodsProbeAnim(snap.current_anim) || IsGoodsProbeAnim(snap.stay_anim_upper) ||
         IsGoodsProbeAnim(snap.stay_anim_lower)) &&
        (snap.current_anim != prev_.current_anim || snap.stay_anim_upper != prev_.stay_anim_upper ||
         snap.stay_anim_lower != prev_.stay_anim_lower)) {
      LogWrite("probe: goods-anim cur=" + std::to_string(prev_.current_anim) + "→" +
               std::to_string(snap.current_anim) + " stay=" + std::to_string(prev_.stay_anim_upper) +
               "/" + std::to_string(prev_.stay_anim_lower) + "→" +
               std::to_string(snap.stay_anim_upper) + "/" + std::to_string(snap.stay_anim_lower));
    }

    if (cfg.laugh_on_empty_flask && AnyAnimEntered(snap, prev_, IsEmptyEstusAnim)) {
      const std::string label = "empty flask (anim/stay=" + std::to_string(snap.current_anim) + "/" +
                                std::to_string(snap.stay_anim_upper) + "/" +
                                std::to_string(snap.stay_anim_lower) + ")";
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

  prev_ = snap;
}

}  // namespace sitcom
