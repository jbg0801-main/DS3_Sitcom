# DS3 Sitcom — pointer research log

Target: **Dark Souls III** PC app **v1.15.x** (Steam). AOBs break on other builds.

Sources: [SoulMemory DarkSouls3.cs](https://github.com/FrankvdStam/SoulSplitter/blob/main/src/SoulMemory/DarkSouls3/DarkSouls3.cs), [TGA DS3 CT](https://github.com/The-Grand-Archives/Dark-Souls-III-CT-TGA), [darksouls3 MenuMan](https://docs.rs/darksouls3/latest/darksouls3/sprj/struct.MenuMan.html), [SoulSplitter Boss flags](https://github.com/FrankvdStam/SoulSplitter/blob/main/src/SoulMemory/DarkSouls3/Boss.cs).

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
| Current anim (TAE) | `+0x1F90 → +0x80 → +0xC8` |
| Stay anims | `+0x1F90 → +0x58 → +0x20 → +0x2A0 / +0x498` |

Death counter: `GameDataMan → +0x98` (Death Num).

DS3 anim encoding (from playtest): values like `10020210` ≈ **TAE 100 × 100000 + behavior** (see Cinders TAE notes).

## Boss flags

Defeat flags from SoulSplitter `Boss` enum (e.g. Iudex `14000800`). Fight-active / cheer flags use **defeat + 1** (EMEVD pattern, e.g. Iudex fight `14000801`). See `kBossPairs[]` in `game_state.cpp`.

Event flag read: SoulMemory `ReadEventFlag` walk ported in `event_flags.cpp` (`SprjEventFlagMan+0x218` regions, FieldArea `+0x10` world-block lookup).

## Area title / scene wipe

Area title cards fire at **map trigger volumes** when you walk into them — not on a timer after bonfire load.

**Primary (visual):** `SprjFadeImp` Title / MapIn fade — TGA chain `[[[[slot]+8]+8]+0]+28`, Title Remain `@+0x38`, MapIn Remain `@+0xF8`.

**MenuMan fallback:** scan `flags[500]` at `+0x32` for EMEVD Display Banner type `8` (Current Location); DSR byte `@+0x104` kept as legacy fallback.

**Seamless walk fallback:** `FieldArea+0x20` world block id (or `area_number`) changes while in gameplay without a loading screen (Fog gate boundaries). Does **not** fire on block/area change at load-end — bonfire warps show the title only when you reach the map trigger.

**Legacy fallback:** `FieldArea` `area_number` change during seamless walk (maps where block id is stable).

AOB: `48 8B 0D ?? ?? ?? ?? 4C 8D 4C 24 38 4C 8D 44 24 48 33 D2` → `SprjFadeImp` (SoulSplitter).

Transition calibration: `log=true`, `[debug] probe_transitions=true` — watch `probe: title-fade`, `probe: world-block-change`, `probe: loading-start/end`.

## Audio notes

- Playback via **waveOut** (`waveOutOpen` / `waveOutWrite` / `WOM_DONE` callback) — isolated from FMOD/PlaySound alias.
- Hit laughs deferred **300 ms** after HP drop.
- Startup smoke-test laugh waits for **title screen** + FMOD SE **stable 2 s** (or 12 s timeout).
- Laugh category gets **1.5×** gain multiplier in `Audio::Play`.

## Item grant hook

TGA ItemGib inner AOB: `8B 02 83 F8 06` — hook at **−0x2F** (MapItemMan grant entry).

Estus restock filter: full ids `0x40000096`–`0x400000A9`, ashen `0x400007E4`–`0x400007EE`.

## Anim TAE ids (fail laughs)

| Event | DS3 id(s) | Notes |
| --- | --- | --- |
| Empty flask | `19001` / `10019001` | ESD 017 empty start anim; no reliable remaining-charge pointer — anim-only |
| Ground pickup | 7520, 7522 | DSR placeholder — confirm with `probe: anim-change` |
| Locked door | 7510 | DSR placeholder |
| Fail cast | 6299, 6399 | DSR placeholder |
| Ladder fall | 1560, 7050–7052 | DSR placeholder |

Enable `log=true`; empty estus logs `probe: anim-change cur=…→…`.

## Playtest checklist

- [ ] Cold boot → title credit, no gameplay laughs
- [ ] Load save → no wipe/cheer/applause storm
- [ ] Hit / death → laugh plays fully in combat (`audio: waveOut play laugh`)
- [ ] Boss fog → cheer; kill → applause
- [ ] New area → scene wipe (banner flag or area transition fallback)
- [ ] Ground pickup → ooh; bonfire estus refill silent
- [ ] Empty estus → fail laugh

## Tested build

| Exe hash | Date | Notes |
| --- | --- | --- |
| _(fill after playtest)_ | | App ver 1.15.x |
