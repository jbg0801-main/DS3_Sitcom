# DS3 Sitcom — pointer / hook recipe

Target: **Dark Souls III** PC app **v1.15.x** (Steam). Addresses are dump VAs at image base `0x140000000`; runtime uses `module_base + (VA - 0x140000000)`.

Primary source: [`MOD_FINDINGS.md`](../MOD_FINDINGS.md) (decompiled game). Do not revive abandoned AOB drink-gates, F23 index-8 probes, or SprjFade RemainTiem wipe guesses.

## Exe

| Key | Value |
| --- | --- |
| Process | `DarkSoulsIII.exe` |
| Bitness | 64-bit |
| Sitcom audio | **waveOut** |
| FMOD | SE volume capture only (`fmod_volume.cpp`) |

## Shared

| Symbol | Dump VA | Notes |
| --- | --- | --- |
| `g_WorldChrMan` | `0x14477FDB8` | Local player `*(wcm + 0x80)` |
| `Chr_IsLocalPlayer` | `0x14088CFF0` | |
| `g_MapNameState` | `0x144763258` | Title ring, boss gauges, place id |
| HP | `chr+0x1F90 → +0x18 → +0xD8` | |

Implemented in [`src/game_hooks.cpp`](../src/game_hooks.cpp).

## Event hooks (live)

| Sitcom event | Hook | Dump VA | Filter |
| --- | --- | --- | --- |
| Empty Estus laugh | Anim enter `50113` / `50114` (`ItemDrinkEmpty`) | — | True empty shake only. Last charged drink is `50110–50112` (quiet). Do **not** use HandleUseItem “count==0” — that fires on the final charge (1→0). |
| Area wipe | `MapName_QueueMenuEvent` | `0x140765F30` | `menu_event_id == 8` |
| Hit laugh | `ChrDamageModule_ApplyDamageToHp` | `0x1409C48C0` | Local; `applied_damage > 0`; HP stays > 0 |
| Death laugh | `Chr_Kill` + lethal ApplyDamageToHp | `0x140888FC0` | Local; dedupe with hit lethal |
| Boss cheer | `Event_SetBossGauge` | `0x1404855A0` | `name_msg_id != -1` |
| Boss applause | gauge handle on kill / `Event_MediumBossDestroy` | `0x140491DF0` | Skip when `args+8 == 1` |
| Ladder fall | `Chr_OnBehaviorEvent` / `ChrCtrl_OnBehaviorEvent` | `0x140996760` / `0x1405D7AB0` | event `0x85`, state 21–23 |

Place id for logs: `g_MapNameState + 0x1A38`. Boss gauge slots: `+0x1AF8`, stride `0x98`, handle at `+0x04`.

**Warp backup wipe:** world-block id change in `events.cpp` (bonfire travel when event 8 is absent).

## Abandoned (do not re-enable without new evidence)

- Estus AOB use-check `B9 43 00 00 00` / drink-gate
- F23 index-8 / clip-player / MapName heap scan
- Empty flask via TAE int `50114` or SoulsGym HKX name `050114`
- SprjFade Title RemainTiem / FMOD place-name wipe as primary
- Event-flag `kBossPairs` cheer/defeat for boss audio

## Secondary (unchanged)

| Event | Signal |
| --- | --- |
| Item ooh | MapItemMan grant hook + pickup anims 7520/7522 |
| Locked door | anim 7510 |
| Fail cast | anim 6299 / 6399 |

## Playtest checklist

- [ ] Empty flask → `empty flask (ChrCtrl_UseItem)`; filled / LB quiet
- [ ] Cemetery ↔ Firelink → `MapName event 8` wipe once
- [ ] Bonfire warp → wipe (event 8 and/or world block)
- [ ] Hit → deferred laugh; death → one laugh
- [ ] Boss bar → cheer; boss die → applause
- [ ] Ladder knock-off (21–23) → laugh; slide quiet
- [ ] Boot: `game_hooks: … armed` lines, no title crash
