# What Scholar had to unlearn

DS3 Sitcom is its own 64-bit overlay for `DarkSoulsIII.exe` 1.15. These notes are from the DS2 Scholar port in `DS2_Sitcom`, where Estus, boss enter/kill, and the area title card all looked right on paper and were wrong in game. Do not copy Scholar addresses, animation ids, or flag ids.

Scholar’s working rules, for when DS3’s current signals misbehave:

## Estus

The filled chug and the empty shake are not two animations you can split with a growing exclusion list. On Scholar the charge helper has two tails: a success path that decrements the charge, and an empty path (`xor ecx, ecx` then the shared setter) that a filled drink never takes. The laugh is a hook on that empty tail only.

Patching the decrement itself clobbered the register that held the new count and made the flask infinite. Steal the fewest bytes on the fail path, replay them, and jump back.

A 20 Hz anim poll is a backup, and a bad one. Scholar’s empty shake is TAE `180202` after `180200 → 900`; the filled drink is `180201`. The poll missed the empty id, and other actions share the lead-in. DS3’s `19001` / `10019001` check in `src/events.cpp`, plus the weapon, block, and swap exclusions around it, is that same trap. `tools/POINTERS.md` already says there is no reliable remaining-charge pointer and the laugh is anim-only. Find the charge function’s empty tail instead, and log one line per use with the field that separates empty from full.

Proof is two filled drinks with no laugh, then one empty drink with one laugh. Turn the anim probe off after the hook is confirmed. Item grants of flask ids (DS3 already filters `0x40000096`–`0x400000A9` and the ashen range) stay silent; that filter is not the empty-use signal.

## Bosses

A boss existing in the loaded area is not the fight starting. Scholar’s first cue was “this chr is in LoadedEnemies.” Bosses preload with the zone, so the cheer fired on arrival. Fog-wall “cleared” bits are world state, not the health bar.

Cheer is the game’s own in-battle id rising from 0. On Scholar that is `IsBossBattle`: EventManager, then BossBattleState, then `ActiveBossBattleId`. DS3 currently cheers when `defeat_flag + 1` rises (`kBossPairs` in `src/game_state.cpp`, consumed in `src/events.cpp`). Keep that only if a log shows the flag rising at the fog and staying set for the whole fight. A flag that is already on because the boss is loaded, or that flips during the death animation, is the wrong edge.

Applause on the defeat-flag rising edge lands under the death sting. Scholar records the flag (and a kill count) while the battle id is set, and does not clap then. It claps once when that id falls back to 0, the player is still alive, and a loot grant landed within 4 seconds. Walking out of the fog, or dying, drops the pending clap. DS3’s `event: boss defeated flag=` path plays `applause.wav` on the flag edge itself.

The flag object is null on the title screen. Resolve it again once the player exists, and do not leave the reader marked ready if the walk failed (an extra pointer hop follows the vtable and every flag reads off forever). Seed flags that are already on when gameplay starts, or loading a cleared game applauds every dead boss.

Confirm on one early boss (Iudex is the short one): cheer when the bar comes up, one applause with the soul, no applause on a death or on walking back out.

## Area title

The card is not the map id, the world-block id, or an FMOD zone event. Scholar wiped on those and got transitions with no card, and missed walks that showed one. DS3 still treats several of them as the wipe in `src/events.cpp`: MenuMan `i32@104`, a scan of `flags[]` for banner type 8, SprjFade title/map-in, `FmodConsumeAreaTransition`, a world-block change while walking, and an `area_number` change while walking. The README still describes the wipe as a map id change after load. Pick the one signal that matches the on-screen card and stop stacking the rest.

The signal that held up is the UI object that displays the place name, and only a change of the **displayed** text id into a real place id. On Scholar that object is `FeSceneMapName`; the id lives at a field on it (SotFS `+0x1C`), written by its tick. Clearing the id to `-1` is not a card. An activate function that runs only on some first loads is not a later walk.

Some sub-areas update that id and show nothing. Scholar ignores `103000` (Heide’s spiral toward No-man’s Wharf) and `102900` (Majula toward Shaded Woods). DS3 will have the same kind of connector. Log `from→to` once per change, walk one boundary that shows a card and one that does not, and ignore the silent ids. A trace flag that dumps the object every tick gets turned off afterward; it will bury the edge.

Bonfire warps show the card when the name is displayed, not when the load finishes and not on a timer after load. Do not wipe because `is_loading` fell.

## What not to import

- Scholar RVAs (`0x1AE0D3`, `FeSceneMapName` `0x684F0`, battle-id offsets `+0x70/+0x88/+0x14`) and TAE `180202`.
- DS2’s 6-digit defeat flags. DS3’s SoulSplitter ids (Iudex `14000800`) stay the flag list.
- Hooking a lock-on HP gauge to detect a boss. It chatters for every enemy.
