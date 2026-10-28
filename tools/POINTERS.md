# DS3 Sitcom — pointer research log

Target: **Dark Souls III** PC app **v1.15.x** (Steam). AOBs break on other builds.

Sources: [SoulMemory DarkSouls3.cs](https://github.com/FrankvdStam/SoulSplitter/blob/main/src/SoulMemory/DarkSouls3/DarkSouls3.cs), [TGA DS3 CT](https://github.com/The-Grand-Archives/Dark-Souls-III-CT-TGA), [darksouls3 MenuMan](https://docs.rs/darksouls3/latest/darksouls3/sprj/struct.MenuMan.html), [SoulSplitter Boss flags](https://github.com/FrankvdStam/SoulSplitter/blob/main/src/SoulMemory/DarkSouls3/Boss.cs).

Estus, boss timing, and the area card hit the same wrong signals on DS2 Scholar. See `tools/DS2_SCHOLAR.md`. Do not copy those addresses.

## Exe

| Key | Value |
| --- | --- |
| Process | `DarkSoulsIII.exe` |
| Bitness | 64-bit |
| Renderer | D3D11 (Present hook for credit) |
| FMOD | `fmod_event64.dll` — `EventSystem::update` one-shot hook |
| Sitcom audio | **waveOut** (not PlaySound — avoids FMOD preemption in combat) |

## Singleton AOBs (implemented in `game_state.cpp` / `event_flags.cpp`)

| Symbol | AOB (CE syntax) | Notes |
| --- | --- | --- |
| WorldChrMan | `48 8B 1D ?? ?? ?? ?? 48 8B F9 48 85 DB ?? ?? 8B 11 85 D2 ?? ?? 8D` | Fallback: SoulMemory playerIns pattern |
| GameDataMan | `48 8B 0D ?? ?? ?? ?? 4C 8D 44 24 40 45 33 C9 48 8B D3 40 88 74 24 28 44 88 74 24 20` | TGA alt: `48 8B 05 ?? ?? ?? ?? 48 85 C0 ?? ?? 48 8B 40 ?? C3` |
| GameMan | `48 8B ?? ?? ?? ?? 04 89 48 28 C3` (+ fallbacks) | Try: TGA+0x04, no-0x04, `48 8B 05 … 89 48 28 C3`. Title: `+0xB1D` IsTitleStart |
| NewMenuSystem | `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 80 B8 ?? ?? ?? ?? ?? 0F 95 C0 C3` | `+0x88`: 2 = main menu / in-game UI, 3 = pause |
| MenuMan | `48 8B 0D ?? ?? ?? ?? 33 C0 48 39 81 ?? ?? ?? ?? 0F 95 C0 C3` | Title fallback `+0x80`; `flags[i]` at `+0x32` for probes only |
| SprjFadeImp | `48 8B 0D ?? ?? ?? ?? 4C 8D 4C 24 38 4C 8D 44 24 48 33 D2` | Area title card fade — see Area title / scene wipe |
| Loading | `C6 05 ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 94 C0 E9` | Byte at resolved RIP target (not slot−4) |
| FieldArea | `48 8B 0D ?? ?? ?? ?? 48 85 C9 74 26 44 8B` | `+0x10` WorldInfoOwner; `+0x20` current world block; `area_number` via first WorldAreaInfo |
| SprjEventFlagMan | `48 8B 0D ?? ?? ?? ?? 44 0F B6 CB 41 B8 07 00 00 00 8B D6` | TGA runtime; fallback init-store pattern; lazy deref at poll |

## Title screen / credit

Priority chain in `game_state.cpp` (all require no loaded player for GameMan path):

1. `GameMan+0xB1D` (IsTitleStart) && `!player_valid`
2. `MenuMan+0x80 == 1` && `!player_valid` (DSR-proven)
3. Heuristic: `!player_valid && !loading && NewMenuSystem+0x88 == 2`

Present hook installs via `CreditEnsureHook()` after `GameState` init (not gated on title). `CreditUpdate` only toggles draw.

## Player vitals (TGA)

Chain: `WorldChrMan → +0x80 (PlayerIns) → +0x1F90 → +0x18 (SprjChrDataModule)`

| Field | Offset |
| --- | --- |
| HP | `+0xD8` |
| Max HP | `+0xE0` |
| Current anim (TAE int) | `+0x1F90 → +0x80 → +0xC8` |
| Stay anims | `+0x1F90 → +0x58 → +0x20 → +0x2A0 / +0x498` |
| Anim **name** string | `+0x1F90 → +0x28 → +0x898` (UTF-16LE HKX state, SoulsGym `PlayerAnimation`) |

Death counter: `GameDataMan → +0x98` (Death Num).

DS3 anim encoding (from playtest): values like `10020210` ≈ **TAE 100 × 100000 + behavior**. Prefer the **name string** for estus (see below); bare ints alone mislabeled empty vs block.

## Boss flags

Defeat flags from SoulSplitter `Boss` enum (e.g. Iudex `14000800`). Fight-active / cheer flags use **defeat + 1** (EMEVD pattern, e.g. Iudex fight `14000801`). See `kBossPairs[]` in `game_state.cpp`.

Event flag read: SoulMemory `ReadEventFlag` walk ported in `event_flags.cpp` (`SprjEventFlagMan+0x218` regions, FieldArea `+0x10` world-block lookup).

## Area title / scene wipe

Cemetery of Ash and Firelink Shrine share **world block 16 / area 21** (`m40`), so block/area edges miss that walk-in. MenuMan banner flag `8` stayed `0` in heartbeats. `FrpgMenuDlgTE_MapName` heap fields chatter and never gave a clean PlaceName edge.

### Abandoned (playtest: zero title hits)

- **F23 index-8 / `F23_open_MapName` AOB** (`41 83 FE 08 75 4E …`) — `F23 any:0` across walks.
- Clip-player name hooks and MapName vtable heap scan in `area_title_hooks.cpp` — disabled; stubs only.

### Live wipe signals (`events.cpp`, 1.5s cooldown)

1. **TGA SprjFade Title `RemainTiem` rising edge** (walk-in cards).  
   Chain (CE last→first): `38, 28, 0, 8, 8` from `SprjFade` — `+0x28` is an **embedded panel** (no deref). Fields on panel: RemainTiem `+0x38`, CurrentColor.A `+0x1C`, MapIn RemainTiem `+0xF8`. Source: TGA CT v3.4 FADE→Title.
2. **FMOD area-transition Event_Start** names matching place/title heuristics (`FmodConsumeAreaTransition`, requires `probe_transitions`).
3. **World-block change** while in gameplay — covers bonfire warps that leave `m40`.

Log lines: `event: area title card (Title RemainTiem a→b)`, `event: area title card (fmod …)`, `event: area title card (world block A→B)`.

PlaceName UTF-16 / `GetSaveMapNameId` exist in the exe (`~0x28b48a8` / `~0x2816738` file offs) but no CE-proven live ID field yet; do not re-enable F23 until proven.

## Audio notes

- Playback via **waveOut** (`waveOutOpen` / `waveOutWrite` / `WOM_DONE` callback) — isolated from FMOD/PlaySound alias.
- Hit laughs deferred **300 ms** after HP drop.
- Startup smoke-test laugh waits for **title screen** + FMOD SE **stable 2 s** (or 12 s timeout).
- Laugh category gets **1.5×** gain multiplier in `Audio::Play`.

## Item grant hook

TGA ItemGib inner AOB: `8B 02 83 F8 06` — hook at **−0x2F** (MapItemMan grant entry).

Estus restock filter: full ids `0x40000096`–`0x400000A9`, ashen `0x400007E4`–`0x400007EE`. Remaining gulps are **not** a trivial TGA field (`ReadEstusCharges` stays −1).

## Empty flask / estus anims

SoulsGym `PlayerAnimation` at `+0x1F90→+0x28→+0x898` is a **UTF-16 HKX state name** (`Idle`, `Move`, `RollingMedium`) — it does **not** contain TAE ids like `050114`. Estus uses the TAE **int** on `current_anim` (same family as fig14 `a000_05011x`).

| Signal | Truth | Notes |
| --- | --- | --- |
| `current_anim` behavior **50114** (or name contains `050114`) | **Empty shake** | [fig14](https://www.fig14.com/ds3/assets.html) `a000_050114`; edge → one fail laugh. |
| Behavior **50110–50112** | Filled drink | No laugh. Playtest: `50110→50111→50112`. |
| Int `19000` / `19100` family (`10019000`…) | **LB block**, not estus | Do not laugh. |
| Bare `50113` | Drink end / other | Appears after filled cycles; **no** laugh. |
| AOB use-check / drink-gate | **Abandoned** | Never fired on empty press. `estus_hooks.cpp` is a stub. |

Enable `log=true`; look for `event: empty flask (50114/050114)` and `probe: filled estus anim=5011x`.

## Other fail-laugh TAE ints

| Event | DS3 id(s) | Notes |
| --- | --- | --- |
| Ground pickup | 7520, 7522 | Confirm with `probe: anim-change` |
| Locked door | 7510 | |
| Fail cast | 6299, 6399 | |
| Ladder fall | 1560, 7050–7052 | |

## Playtest checklist

- [ ] Cold boot → title credit, no gameplay laughs
- [ ] Load save → no wipe/cheer/applause storm
- [ ] Hit / death → laugh plays fully in combat (`audio: waveOut play laugh`)
- [ ] Boss fog → cheer; kill → applause
- [ ] Ground pickup → ooh; bonfire estus refill silent
- [ ] LB block → no empty laugh; log shows block / `19000` family, not `050114`
- [ ] Empty estus → `anim-name` contains `050114` and one fail laugh
- [ ] Filled drink → `050110`–`050112`, no laugh
- [ ] Walk Cemetery ↔ Firelink → `Title RemainTiem` wipe once
- [ ] Bonfire warp to another block → one world-block wipe, not two

## Tested build

| Exe hash | Date | Notes |
| --- | --- | --- |
| _(fill after playtest)_ | | App ver 1.15.x |
