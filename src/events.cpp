#include "events.h"

#include "game_hooks.h"
#include "game_state.h"
#include "item_hooks.h"
#include "log.h"

#include <windows.h>

#include <cstring>
#include <string>

namespace sitcom {
namespace {

bool IsPickupAnim(std::int32_t anim) {
  return anim == 7520 || anim == 7522;
}

bool IsFailCastAnim(std::int32_t anim) {
  return anim == 6299 || anim == 6399;
}

// Charged drink chains (suppress laugh on the trailing empty-shake frames).
bool IsHpEstusDrinkAnim(std::int32_t anim) {
  return anim >= 50110 && anim <= 50112;
}
bool IsAshenEstusDrinkAnim(std::int32_t anim) {
  return anim >= 50140 && anim <= 50141;  // 50142 is ashen empty-shake, not drink
}

// HP empty shake: 50113/50114. Ashen empty shake: 50142/50143.
// Each flask is independent — laugh when THAT flask's empty anim starts from
// outside its own drink chain (ashen charges must not block HP empty, etc.).
bool IsHpEmptyEstusAnim(std::int32_t anim) {
  return anim == 50113 || anim == 50114;
}
bool IsAshenEmptyEstusAnim(std::int32_t anim) {
  return anim == 50142 || anim == 50143;
}

bool AnimChanged(const GameSnapshot& snap, const GameSnapshot& prev) {
  return snap.current_anim != prev.current_anim || snap.stay_anim_upper != prev.stay_anim_upper ||
         snap.stay_anim_lower != prev.stay_anim_lower;
}

bool AnimEntered(const GameSnapshot& snap, const GameSnapshot& prev, bool (*pred)(std::int32_t)) {
  return snap.anim_valid && prev.anim_valid && pred(snap.current_anim) && !pred(prev.current_anim);
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
  pending_wipe_ = false;
  last_shown_place_id_ = -1;
  last_anim_probe_ms_ = 0;
  loading_from_gameplay_ = false;
  spawn_load_seen_ = false;
  block_baseline_seen_ = false;
  last_seen_block_ = -1;
  last_seen_area_ = 255;
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
  const bool can_wipe = cfg.wipe_on_area_title && !snap.on_title_screen && snap.in_gameplay &&
                        !snap.is_loading && (now - last_wipe_ms_) >= 1500;

  // MOD_FINDINGS: QueueMenu(8) when it fires. In practice the live signal is
  // g_MapNameState+0x1A3C (last shown place id) — QueueMenu is often skipped.
  const int title_hits = GameHooksConsumeAreaTitle();
  if (title_hits > 0) {
    pending_wipe_ = true;
    LogWrite("event: title card queued (MapName event 8 place=" +
             std::to_string(GameHooksLastPlaceNameId()) + " x" + std::to_string(title_hits) + ")");
  }

  // Title screen / main menu: invalidate so the next spawn's Cemetery/Firelink
  // card counts as a change even if the place id matches the previous session.
  if (snap.on_title_screen) {
    last_shown_place_id_ = -1;
  }

  // Place-id edge. Keep last id across loads so bonfire warps only wipe when
  // the destination title writes a new id (not when the origin id is first
  // observed again after load). Do NOT arm on world-block alone.
  if (!snap.is_loading) {
    const std::int32_t shown_place = GameHooksReadShownPlaceNameId();
    if (shown_place != -1 && shown_place != last_shown_place_id_) {
      pending_wipe_ = true;
      LogWrite("event: title card place id " + std::to_string(last_shown_place_id_) + "→" +
               std::to_string(shown_place));
      last_shown_place_id_ = shown_place;
    }
  }

  if (snap.world_block_valid && snap.world_block_id >= 0) {
    last_seen_block_ = snap.world_block_id;
  }

  if (can_wipe && pending_wipe_) {
    LogWrite("event: area title card wipe (place=" + std::to_string(last_shown_place_id_) + ")");
    audio.Play(SoundCategory::SceneWipe);
    last_wipe_ms_ = now;
    pending_wipe_ = false;
  }

  constexpr int kGoodsCategory = 0x40000000;

  if (pending_hit_laugh_ms_ != 0 && now >= pending_hit_laugh_ms_) {
    pending_hit_laugh_ms_ = 0;
    if (cfg.laugh_on_hit && snap.in_gameplay) {
      const auto cooldown_ms = static_cast<std::uint64_t>(cfg.laugh_hit_seconds * 1000.0f);
      if (now - last_laugh_hit_ms_ >= cooldown_ms) {
        LogWrite("event: player hit (ApplyDamageToHp)");
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

  if (snap.in_gameplay && snap.world_block_valid && snap.world_block_id >= 0) {
    block_baseline_seen_ = true;
  }

  if (!snap.in_gameplay) {
    if (prev_.in_gameplay) {
      flags_seeded_ = false;
      gameplay_seeded_ms_ = 0;
      applause_armed_ = false;
      armed_defeat_flag_ = 0;
      pending_hit_laugh_ms_ = 0;
    }
    // Still drain death/hit outside gameplay? Skip — require in_gameplay for reactions.
    advance_history();
    return;
  }

  if (!flags_seeded_) {
    flags_seeded_ = true;
    gameplay_seeded_ms_ = now;
    advance_history();
    LogWrite("event: seeded baselines in gameplay");
    return;
  }

  // MOD_FINDINGS: ApplyDamageToHp / Chr_Kill. Snapshot HP drop covers fall /
  // poison / hook misses (Proton IsLocalPlayer false negatives).
  const int deaths = GameHooksConsumeDeath();
  const int hits = GameHooksConsumeHit();
  const bool hp_lethal = has_prev_ && prev_.player_valid && snap.player_valid &&
                         prev_.player_hp > 0 && snap.player_hp <= 0;
  const bool hp_hit = has_prev_ && prev_.player_valid && snap.player_valid &&
                      snap.player_hp > 0 && snap.player_hp < prev_.player_hp;
  if ((deaths > 0 || hp_lethal) && cfg.laugh_on_death) {
    LogWrite("event: player death (hook=" + std::to_string(deaths) +
             " hp=" + std::to_string(prev_.player_hp) + "→" + std::to_string(snap.player_hp) + ")");
    audio.Play(SoundCategory::Laugh);
    pending_hit_laugh_ms_ = 0;
    last_laugh_hit_ms_ = now;
  } else if ((hits > 0 || hp_hit) && cfg.laugh_on_hit) {
    const auto cooldown_ms = static_cast<std::uint64_t>(cfg.laugh_hit_seconds * 1000.0f);
    if (now - last_laugh_hit_ms_ >= cooldown_ms && pending_hit_laugh_ms_ == 0) {
      pending_hit_laugh_ms_ = now + kHitLaughDeferMs;
      if (hp_hit && hits == 0) {
        LogWrite("event: player hit armed (hp " + std::to_string(prev_.player_hp) + "→" +
                 std::to_string(snap.player_hp) + ")");
      }
    }
  }

  // Boss bar: gauge slot becomes active. Defeat: gauged chr HP→0 and/or slot clear.
  const int cheers = GameHooksConsumeBossCheer();
  if (cfg.cheer_on_boss_bar && cheers > 0) {
    LogWrite("event: boss fight start (gauge slot x" + std::to_string(cheers) + ")");
    audio.Play(SoundCategory::Cheer);
    applause_armed_ = true;
  }
  const int applause = GameHooksConsumeBossApplause();
  // Only after a cheer this fight — ignores slot clears from death/warp/fail.
  if (cfg.applause_on_boss_death && applause > 0 && applause_armed_) {
    LogWrite("event: boss defeated (gauge kill/clear x" + std::to_string(applause) + ")");
    audio.Play(SoundCategory::Applause);
    applause_armed_ = false;
  } else if (applause > 0 && !applause_armed_) {
    LogWrite("event: boss gauge clear ignored (applause not armed)");
  }

  (void)GameHooksConsumeEmptyEstus();  // UseItem hooks disarmed; keep drain harmless.

  // Ladder act byte poll (21/22/23). No OnBehaviorEvent hook — trampoline unsafe.
  const int ladder = GameHooksConsumeLadderFall();
  if (cfg.laugh_on_ladder_fall && ladder > 0) {
    TryFailLaugh(now, &last_fail_laugh_ms_, cfg.laugh_hit_seconds, audio,
                 "ladder fall (act byte)");
  }

  if (snap.anim_valid && prev_.anim_valid) {
    if (cfg.log && AnimChanged(snap, prev_) && now - last_anim_probe_ms_ >= kAnimProbeMinMs) {
      LogWrite("probe: anim-change cur=" + std::to_string(prev_.current_anim) + "→" +
               std::to_string(snap.current_anim) + " stay=" + std::to_string(prev_.stay_anim_upper) +
               "/" + std::to_string(prev_.stay_anim_lower) + "→" +
               std::to_string(snap.stay_anim_upper) + "/" + std::to_string(snap.stay_anim_lower) +
               " name=" + (snap.anim_name_valid ? snap.anim_name : "-"));
      last_anim_probe_ms_ = now;
    }
    // Per-flask use-at-zero. Suppress only when trailing THAT flask's drink chain.
    // Ashen last sip often ends 50141→50142→50113 — suppress both edges via drink check
    // on 50141 and by treating 50113 after 50142 as still in the ashen chain.
    if (cfg.laugh_on_empty_flask) {
      const bool from_ashen_chain = IsAshenEstusDrinkAnim(prev_.current_anim) ||
                                    prev_.current_anim == 50142;
      if (AnimEntered(snap, prev_, IsHpEmptyEstusAnim) && !IsHpEstusDrinkAnim(prev_.current_anim) &&
          !from_ashen_chain) {
        TryFailLaugh(now, &last_fail_laugh_ms_, cfg.laugh_hit_seconds, audio,
                     "empty HP estus (use at 0)");
      }
      if (AnimEntered(snap, prev_, IsAshenEmptyEstusAnim) &&
          !IsAshenEstusDrinkAnim(prev_.current_anim)) {
        TryFailLaugh(now, &last_fail_laugh_ms_, cfg.laugh_hit_seconds, audio,
                     "empty ashen estus (use at 0)");
      }
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
  }

  advance_history();
}

}  // namespace sitcom
