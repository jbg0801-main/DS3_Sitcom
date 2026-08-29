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

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace sitcom
