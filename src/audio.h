#pragma once

#include <string>

namespace sitcom {

enum class SoundCategory {
  Laugh,
  Cheer,
  Applause,
  Ooh,
  SceneWipe,
};

class Audio {
 public:
  bool Init(const std::wstring& sounds_dir, float volume);
  void Shutdown();

  void SetVolume(float volume);
  void SetGameSfxVolume(float game_sfx_0_to_1);

  void Play(SoundCategory category);
  // Title-menu smoke test: PlaySound path avoids waveOut being ducked by title BGM init.
  void PlayTitleSmokeTest(SoundCategory category);

  // Called from winmm waveOut callback (audio.cpp).
  void OnWaveOutDone(void* hwo);

 private:
  struct Impl;
  static void FreePlayback(Impl* impl);
  static void FreeLaughPlayback(Impl* impl);
  Impl* impl_ = nullptr;
};

}  // namespace sitcom
