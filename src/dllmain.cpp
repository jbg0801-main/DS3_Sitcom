#include "audio.h"
#include "config.h"
#include "credit.h"
#include "dinput8_proxy.h"
#include "events.h"
#include "fmod_volume.h"
#include "game_state.h"
#include "item_hooks.h"
#include "log.h"
#include "paths.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <string>

namespace {

HMODULE g_real_dinput = nullptr;
std::atomic<bool> g_running{false};
HANDLE g_worker_thread = nullptr;
HANDLE g_stop_event = nullptr;

DWORD WINAPI WorkerMain(LPVOID) {
  using namespace sitcom;

  WaitForSingleObject(g_stop_event, 1500);
  if (!g_running) {
    return 0;
  }

  const std::wstring dll_dir = GetDllDirectory();
  const std::wstring sitcom_dir = JoinPath(dll_dir, L"sitcom");
  EnsureDirectory(sitcom_dir);

  const std::wstring log_path = JoinPath(sitcom_dir, L"sitcom.log");
  const std::wstring ini_path = JoinPath(sitcom_dir, L"config.ini");

  if (!LogBootstrap(log_path)) {
    LogBootstrap(JoinPath(dll_dir, L"sitcom.log"));
  }

  LogWrite("worker: start");
  LogWrite("worker: dll_dir=" + WideToUtf8(dll_dir));
  LogWrite("worker: sitcom_dir=" + WideToUtf8(sitcom_dir));
  LogWrite(std::string("worker: config.ini exists=") + (FileExists(ini_path) ? "yes" : "NO"));

  WaitForSingleObject(g_stop_event, 1500);
  if (!g_running) {
    return 0;
  }

  Config cfg;
  LoadConfig(ini_path, cfg);

  if (!FileExists(ini_path)) {
    cfg.log = true;
    LogWrite("worker: config missing — using defaults with logging on");
  }
  LogInit(log_path, cfg.log);
  if (cfg.log) {
    LogWrite("worker: logging enabled");
  }

  LogWrite(std::string("worker: enabled=") + (cfg.enabled ? "1" : "0") +
           " volume=" + std::to_string(cfg.volume) + " poll_hz=" + std::to_string(cfg.poll_hz));

  if (!cfg.enabled) {
    LogWrite("worker: disabled via config");
    return 0;
  }

  Audio audio;
  const std::wstring sounds = JoinPath(sitcom_dir, Utf8ToWide(cfg.sounds_dir));
  LogWrite(std::string("worker: sounds_dir exists=") + (FileExists(JoinPath(sounds, L"laugh.wav")) ||
                                                                FileExists(JoinPath(sounds, L"laugh_01.wav"))
                                                            ? "yes"
                                                            : "check"));
  if (!audio.Init(sounds, cfg.volume)) {
    LogWrite("worker: audio init failed");
    return 0;
  }

  GameState game;
  int failures = 0;
  while (g_running && !game.Init()) {
    LogWrite("worker: waiting for game patterns...");
    if (WaitForSingleObject(g_stop_event, 2000) != WAIT_TIMEOUT) {
      audio.Shutdown();
      return 0;
    }
    if (++failures > 60) {
      LogWrite("worker: giving up resolving patterns");
      audio.Shutdown();
      return 0;
    }
  }
  LogWrite("worker: GameState ready");
  CreditEnsureHook();

  ItemHooksInit();

  // Attach FMOD only after the game is up — IAT/inline hooks, no LoadLibrary, no probes.
  if (FmodVolumeInit()) {
    float se = 1.f;
    if (FmodTryGetSfxVolume(&se)) {
      audio.SetGameSfxVolume(se);
    }
  } else {
    LogWrite("fmod: not ready yet — will retry while polling");
  }

  EventDetector events;
  const DWORD poll_ms = static_cast<DWORD>(1000 / (cfg.poll_hz > 0 ? cfg.poll_hz : 20));
  LogWrite("worker: polling every " + std::to_string(poll_ms) + "ms");

  std::uint64_t ticks = 0;
  float last_logged_sfx = -1.f;
  bool startup_laugh_pending = true;
  std::uint64_t last_sfx_change_ms = 0;
  const std::uint64_t worker_start_ms = static_cast<std::uint64_t>(GetTickCount64());
  while (g_running) {
    const GameSnapshot snap = game.Read();
    const auto now_ms = static_cast<std::uint64_t>(GetTickCount64());

    float sfx = 1.f;
    const bool sfx_ok = FmodTryGetSfxVolume(&sfx);
    if (sfx_ok) {
      audio.SetGameSfxVolume(sfx);
      if (std::fabs(sfx - last_logged_sfx) > 0.01f) {
        LogWrite("audio: FMOD SE volume=" + std::to_string(sfx));
        last_logged_sfx = sfx;
        last_sfx_change_ms = now_ms;
      }
    } else {
      audio.SetGameSfxVolume(1.f);
    }

    if (startup_laugh_pending) {
      const auto elapsed = now_ms - worker_start_ms;
      const auto sfx_stable_ms =
          last_sfx_change_ms != 0 ? (now_ms - last_sfx_change_ms) : 0;
      const bool sfx_settled = sfx_ok && sfx_stable_ms >= 8000;
      const bool title_ready = snap.on_title_screen && !snap.is_loading;
      if (title_ready && sfx_settled) {
        LogWrite("worker: playing startup laugh smoke-test (title+sfx settled 8s, PlaySound)");
        audio.PlayTitleSmokeTest(SoundCategory::Laugh);
        startup_laugh_pending = false;
      } else if (elapsed >= 20000) {
        LogWrite("worker: playing startup laugh smoke-test (timeout)");
        audio.PlayTitleSmokeTest(SoundCategory::Laugh);
        startup_laugh_pending = false;
      }
    }

    CreditUpdate(snap.on_title_screen);

    if ((ticks++ % 100) == 0) {
      char sfx_buf[32];
      if (sfx_ok) {
        snprintf(sfx_buf, sizeof(sfx_buf), "%.2f", sfx);
      } else {
        snprintf(sfx_buf, sizeof(sfx_buf), "?");
      }
      LogWrite(std::string("worker: heartbeat in_game=") + std::to_string(snap.in_gameplay) +
               " title=" + std::to_string(snap.on_title_screen) +
               " gm_title=" + std::to_string(snap.game_man_title) +
               " menu=" + std::to_string(snap.menu_state) +
               " player_valid=" + std::to_string(snap.player_valid) +
               " hp=" + std::to_string(snap.player_hp) + "/" + std::to_string(snap.player_max_hp) +
               " boss_fight=" + std::to_string(snap.boss_fight_active) +
               " cheer=" + std::to_string(snap.boss_cheer_flag) +
               " title_fade=" + std::to_string(snap.title_fade_remain) + "/" +
               std::to_string(snap.title_fade_alpha) +
               " mapin=" + std::to_string(snap.mapin_fade_remain) + "/" +
               std::to_string(snap.mapin_fade_alpha) +
               " block=" + std::to_string(snap.world_block_id) +
               " area=" + std::to_string(snap.area_number) +
               " banner=" + std::to_string(static_cast<unsigned>(snap.banner_message)) +
               " banner_idx=" + std::to_string(snap.banner_flag_idx) +
               " sfx=" + sfx_buf +
               " loading=" + std::to_string(snap.is_loading) +
               " estus=" + std::to_string(snap.estus_charges) +
               " anim=" + std::to_string(snap.current_anim) +
               " stay=" + std::to_string(snap.stay_anim_upper) + "/" +
               std::to_string(snap.stay_anim_lower));
    }
    events.Update(snap, cfg, audio);
    FmodSetProbeTransitions(cfg.probe_transitions);
    if (WaitForSingleObject(g_stop_event, poll_ms) != WAIT_TIMEOUT) {
      break;
    }
  }

  CreditShutdown();
  ItemHooksShutdown();
  game.Shutdown();
  audio.Shutdown();
  FmodVolumeShutdown();
  LogWrite("worker: stop");
  return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      DisableThreadLibraryCalls(hModule);
      sitcom::SetDllModule(hModule);
      g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

      DirectInput8Create_t create = nullptr;
      if (!LoadRealDinput8(&g_real_dinput, &create)) {
        return FALSE;
      }

      g_running = true;
      // CreateThread (not std::thread) — safer from DllMain under loader lock / Wine.
      g_worker_thread = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
      break;
    }
    case DLL_PROCESS_DETACH: {
      g_running = false;
      if (g_stop_event) {
        SetEvent(g_stop_event);
      }
      if (g_worker_thread) {
        WaitForSingleObject(g_worker_thread, 5000);
        CloseHandle(g_worker_thread);
        g_worker_thread = nullptr;
      }
      UnloadRealDinput8(g_real_dinput);
      g_real_dinput = nullptr;
      if (g_stop_event) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
      }
      break;
    }
    default:
      break;
  }
  return TRUE;
}
