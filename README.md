# DS3 Sitcom

Sitcom canned-audience overlay for **Dark Souls III** (PC, app v1.15.x).

Plays local WAV clips when:

- the player takes damage or dies (laughter)
- a boss fight starts via EMEVD fight-active flags (cheering)
- a boss defeated flag flips (applause)
- the loaded map/area id changes after entering gameplay (scene wipe)
- an item is granted via MapItemMan or pickup anim (**ooh**)
- fail states: empty flask, locked door, no spell, ladder fall (laughter)

## Install

1. Copy `dinput8.dll` and the `sitcom/` folder into the game directory (next to `DarkSoulsIII.exe`).
2. Launch the game normally.
3. **Linux / Proton** launch options:

```text
WINEDLLOVERRIDES="dinput8.dll=n,b" %command%
```

4. Put your WAVs in `sitcom/sounds/` using the names below.
5. Tweak `sitcom/config.ini` as needed. Set `log=true` to write `sitcom/sitcom.log`.

**Offline / single-player recommended.** Injected DLLs can interact badly with online play (softban risk).

If another mod already ships `dinput8.dll`, list it in `sitcom/chainload.txt` or the `[chainload] dlls=` line in `config.ini` (one DLL per line / comma-separated).

## Sound files

Preferred format: **WAV, PCM signed 16-bit, 44100 Hz** (48000 OK). Stereo is fine.

The loader picks randomly among `name.wav` and `name_XX.wav` / `name-XX.wav` for each category when multiple files are present.

| Prefix / name | Event |
| --- | --- |
| `laugh.wav` / `laugh_XX.wav` | Hit / death / fail laughs |
| `cheer.wav` / `cheer_XX.wav` | Boss fight start |
| `applause.wav` / `applause_XX.wav` | Boss defeated |
| `ooh.wav` / `ooh_XX.wav` | Item pickup |
| `scene_wipe.wav` / `scene_wipe_XX.wav` | Area title / map change |

## Build (Linux → Windows DLL)

Requires MinGW-w64 (`x86_64-w64-mingw32-g++`) and CMake.

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake
cmake --build build -j
```

Or: `bash scripts/build-podman.sh` (builds + packs `dist/DS3_Sitcom.zip`).

## How it works

`dinput8.dll` proxies `DirectInput8Create` to the system DLL and starts a worker thread that:

1. Resolves WorldChrMan, GameDataMan, SprjEventFlagMan, and related singletons via AOB scans (SoulMemory / TGA sources)
2. Polls ~20 Hz for HP, boss flags, map id, anims, and item grants
3. Plays WAVs through **winmm `PlaySound`** with PCM gain (`config volume` × in-game **Sound Effect** / FMOD `SE` when captured)
4. Draws a title-menu credit (`Sitcom mod by jbg0801 2026` plus dedication)

See [tools/POINTERS.md](tools/POINTERS.md) for pointer notes and playtest TODOs.
