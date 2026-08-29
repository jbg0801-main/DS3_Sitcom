# DS3 Sitcom — pointer research log

Target: **Dark Souls III** PC app **v1.15.x** (Steam). AOBs break on other builds.

Sources: [SoulMemory DarkSouls3.cs](https://github.com/FrankvdStam/SoulSplitter/blob/main/src/SoulMemory/DarkSouls3/DarkSouls3.cs), [TGA DS3 CT](https://github.com/The-Grand-Archives/Dark-Souls-III-CT-TGA), [SoulSplitter Boss flags](https://github.com/FrankvdStam/SoulSplitter/blob/main/src/SoulMemory/DarkSouls3/Boss.cs).

## Exe

| Key | Value |
| --- | --- |
| Process | `DarkSoulsIII.exe` |
| Bitness | 64-bit |
| Renderer | D3D11 (Present hook for credit) |
| FMOD | `fmod_event64.dll` — `EventSystem::update` one-shot hook |

## Singleton AOBs (implemented in `game_state.cpp` / `event_flags.cpp`)

| Symbol | AOB (CE syntax) | Notes |
| --- | --- | --- |
| WorldChrMan | `48 8B 1D ?? ?? ?? ?? 48 8B F9 48 85 DB ?? ?? 8B 11 85 D2 ?? ?? 8D` | Fallback: SoulMemory playerIns pattern |
| GameDataMan | `48 8B 0D ?? ?? ?? ?? 4C 8D 44 24 40 45 33 C9 48 8B D3 40 88 74 24 28 44 88 74 24 20` | TGA alt: `48 8B 05 ?? ?? ?? ?? 48 85 C0 ?? ?? 48 8B 40 ?? C3` |
| GameMan | `48 8B ?? ?? ?? ?? 04 89 48 28 C3` (+ fallbacks) | Try: TGA+0x04, no-0x04, `48 8B 05 … 89 48 28 C3`. Title: `+0xB1D` IsTitleStart |
| NewMenuSystem | `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 80 B8 ?? ?? ?? ?? ?? 0F 95 C0 C3` | `+0x88`: 2 = main menu / in-game UI, 3 = pause |
| MenuMan | `48 8B 0D ?? ?? ?? ?? 33 C0 48 39 81 ?? ?? ?? ?? 0F 95 C0 C3` | Mask **must** be 20 chars (`xxx????xxxxx????xxxx`). Banner at `+0x104`; title at `+0x80` |
| Loading | `C6 05 ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 94 C0 E9` | Byte at resolved RIP target (not slot−4) |
| FieldArea | `48 8B 0D ?? ?? ?? ?? 48 85 C9 74 26 44 8B` | Non-null deref = in-world; event-flag world block at `+0x10` |
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

## Boss flags

Defeat flags from SoulSplitter `Boss` enum (e.g. Iudex `14000800`). Fight-active / cheer flags use **defeat + 1** (EMEVD pattern, e.g. Iudex fight `14000801`). See `kBossPairs[]` in `game_state.cpp`.

Event flag read: SoulMemory `ReadEventFlag` walk ported in `event_flags.cpp` (`SprjEventFlagMan+0x218` regions, FieldArea `+0x10` world-block lookup).

## Area title / scene wipe

DS3 uses **MenuMan+0x104** Display Banner id (same EMEVD semantics as DSR; type **8** = Current Location / area name card). Rising edge while `in_gameplay && !loading` triggers scene wipe.

**Not** `FieldArea → +0x18 → +0xE8 → +0x40` — TGA uses that chain for camera position floats.

## Audio notes

- Hit laughs deferred **300 ms** after HP drop (avoids FMOD impact SFX masking winmm).
- Startup smoke-test laugh deferred until FMOD SE captured or **3 s** after worker start.
- Laugh category gets **1.5×** gain multiplier in `Audio::Play`.

## Item grant hook

TGA ItemGib inner AOB: `8B 02 83 F8 06` — hook at **−0x2F** (MapItemMan grant entry).

Estus restock filter: full ids `0x40000096`–`0x400000A9`, ashen `0x400007E4`–`0x400007EE`.

## Anim TAE ids (fail laughs) — confirm in-game

Placeholder values copied from DSR until CE playtest pins DS3 ids. Enable `log=true` and watch `probe: goods-anim` lines.

| Event | Placeholder TAE | CE action |
| --- | --- | --- |
| Ground pickup | 7520, 7522 | Pick up item |
| Locked door | 7510 | Try locked door |
| Fail cast | 6299, 6399 | Cast with 0 FP |
| Empty flask | 7588, 7589 | Drink empty estus |
| Ladder fall | 1560, 7050–7052 | Fall off ladder |

## Playtest checklist

- [ ] Cold boot → title credit, no gameplay laughs
- [ ] Load save → no wipe/cheer/applause storm
- [ ] Hit / death → laugh (audible ~300 ms after hit)
- [ ] Boss fog → cheer; kill → applause
- [ ] New area → one scene wipe
- [ ] Ground pickup → ooh; bonfire estus refill silent
- [ ] Fail laughs (after TAE ids confirmed)

## Tested build

| Exe hash | Date | Notes |
| --- | --- | --- |
| _(fill after playtest)_ | | App ver 1.15.x |
