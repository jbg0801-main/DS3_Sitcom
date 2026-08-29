#include "audio.h"

#include "log.h"
#include "paths.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mmsystem.h>
#include <random>
#include <string>
#include <vector>

namespace sitcom {
namespace {

void CollectPrefix(const std::wstring& dir, const std::wstring& prefix,
                   std::vector<std::wstring>& out) {
  const std::wstring pattern = JoinPath(dir, prefix + L"*");
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      continue;
    }
    std::wstring name = fd.cFileName;
    const std::wstring stem = name.substr(0, name.find_last_of(L'.'));
    const bool ext_ok = name.size() >= 4 &&
                        (_wcsicmp(name.c_str() + name.size() - 4, L".wav") == 0);
    if (!ext_ok) {
      continue;
    }
    if (stem == prefix ||
        (stem.size() > prefix.size() && stem.compare(0, prefix.size(), prefix) == 0 &&
         (stem[prefix.size()] == L'_' || stem[prefix.size()] == L'-'))) {
      out.push_back(JoinPath(dir, name));
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end());
}

int PickIndex(std::mt19937& rng, int count, int last) {
  if (count <= 0) {
    return -1;
  }
  if (count == 1) {
    return 0;
  }
  std::uniform_int_distribution<int> dist(0, count - 1);
  int idx = dist(rng);
  if (idx == last) {
    idx = (idx + 1) % count;
  }
  return idx;
}

bool LoadFile(const std::wstring& path, std::vector<std::uint8_t>& out) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024) {
    CloseHandle(f);
    return false;
  }
  out.resize(static_cast<std::size_t>(sz.QuadPart));
  DWORD read = 0;
  const BOOL ok =
      ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
  CloseHandle(f);
  return ok && read == out.size();
}

bool ApplyGainToWav(std::vector<std::uint8_t>& wav, float gain) {
  if (wav.size() < 44 || gain >= 0.999f) {
    return gain > 0.001f;
  }
  if (gain <= 0.001f) {
    return false;
  }
  if (memcmp(wav.data(), "RIFF", 4) != 0 || memcmp(wav.data() + 8, "WAVE", 4) != 0) {
    return true;
  }

  std::uint16_t audio_format = 1;
  std::uint16_t bits = 16;
  std::size_t data_off = 0;
  std::size_t data_size = 0;

  std::size_t off = 12;
  while (off + 8 <= wav.size()) {
    const char* id = reinterpret_cast<const char*>(wav.data() + off);
    const auto chunk_size = *reinterpret_cast<const std::uint32_t*>(wav.data() + off + 4);
    const std::size_t payload = off + 8;
    if (payload > wav.size()) {
      break;
    }
    if (memcmp(id, "fmt ", 4) == 0 && chunk_size >= 16 && payload + 16 <= wav.size()) {
      audio_format = *reinterpret_cast<const std::uint16_t*>(wav.data() + payload);
      bits = *reinterpret_cast<const std::uint16_t*>(wav.data() + payload + 14);
    } else if (memcmp(id, "data", 4) == 0) {
      data_off = payload;
      data_size = std::min<std::size_t>(chunk_size, wav.size() - payload);
      break;
    }
    std::size_t step = 8 + static_cast<std::size_t>(chunk_size);
    if (chunk_size & 1) {
      ++step;
    }
    off += step;
  }

  if (data_off == 0 || data_size == 0 || audio_format != 1) {
    return true;
  }

  if (bits == 16) {
    auto* samples = reinterpret_cast<std::int16_t*>(wav.data() + data_off);
    const std::size_t n = data_size / 2;
    for (std::size_t i = 0; i < n; ++i) {
      const float v = static_cast<float>(samples[i]) * gain;
      const int clamped = static_cast<int>(std::lround(v));
      samples[i] = static_cast<std::int16_t>(std::max(-32768, std::min(32767, clamped)));
    }
  } else if (bits == 8) {
    auto* samples = wav.data() + data_off;
    for (std::size_t i = 0; i < data_size; ++i) {
      const float centered = (static_cast<float>(samples[i]) - 128.f) * gain + 128.f;
      const int clamped = static_cast<int>(std::lround(centered));
      samples[i] = static_cast<std::uint8_t>(std::max(0, std::min(255, clamped)));
    }
  }
  return true;
}

}  // namespace

struct Audio::Impl {
  float volume = 1.f;
  float game_sfx = 1.f;
  std::mt19937 rng{std::random_device{}()};
  std::vector<std::wstring> laugh;
  std::vector<std::wstring> cheer;
  std::vector<std::wstring> applause;
  std::vector<std::wstring> wipe;
  std::vector<std::wstring> ooh;
  int last_laugh = -1;
  int last_cheer = -1;
  int last_applause = -1;
  int last_wipe = -1;
  int last_ooh = -1;
  std::vector<std::uint8_t> play_buf;
};

bool Audio::Init(const std::wstring& sounds_dir, float volume) {
  Shutdown();
  impl_ = new Impl();
  impl_->volume = volume;

  CollectPrefix(sounds_dir, L"laugh", impl_->laugh);
  CollectPrefix(sounds_dir, L"cheer", impl_->cheer);
  CollectPrefix(sounds_dir, L"applause", impl_->applause);
  CollectPrefix(sounds_dir, L"scene_wipe", impl_->wipe);
  CollectPrefix(sounds_dir, L"ooh", impl_->ooh);

  LogWrite("audio: sounds_dir=" + WideToUtf8(sounds_dir));
  LogWrite("audio: loaded laugh=" + std::to_string(impl_->laugh.size()) +
           " cheer=" + std::to_string(impl_->cheer.size()) +
           " applause=" + std::to_string(impl_->applause.size()) +
           " wipe=" + std::to_string(impl_->wipe.size()) +
           " ooh=" + std::to_string(impl_->ooh.size()));
  return true;
}

void Audio::Shutdown() {
  if (!impl_) {
    return;
  }
  PlaySoundW(nullptr, nullptr, 0);
  delete impl_;
  impl_ = nullptr;
}

void Audio::SetVolume(float volume) {
  if (!impl_) {
    return;
  }
  impl_->volume = std::max(0.f, std::min(1.f, volume));
}

void Audio::SetGameSfxVolume(float game_sfx_volume) {
  if (!impl_) {
    return;
  }
  impl_->game_sfx = std::max(0.f, std::min(1.f, game_sfx_volume));
}

void Audio::Play(SoundCategory category) {
  if (!impl_) {
    return;
  }

  std::vector<std::wstring>* list = nullptr;
  int* last = nullptr;
  const char* label = "";
  switch (category) {
    case SoundCategory::Laugh:
      list = &impl_->laugh;
      last = &impl_->last_laugh;
      label = "laugh";
      break;
    case SoundCategory::Cheer:
      list = &impl_->cheer;
      last = &impl_->last_cheer;
      label = "cheer";
      break;
    case SoundCategory::Applause:
      list = &impl_->applause;
      last = &impl_->last_applause;
      label = "applause";
      break;
    case SoundCategory::SceneWipe:
      list = &impl_->wipe;
      last = &impl_->last_wipe;
      label = "scene_wipe";
      break;
    case SoundCategory::Ooh:
      list = &impl_->ooh;
      last = &impl_->last_ooh;
      label = "ooh";
      break;
  }

  const int idx = PickIndex(impl_->rng, static_cast<int>(list->size()), *last);
  if (idx < 0) {
    LogWrite(std::string("audio: no clips for ") + label);
    return;
  }
  *last = idx;
  const std::wstring& path = (*list)[idx];
  if (!FileExists(path)) {
    LogWrite(std::string("audio: missing file for ") + label + ": " + WideToUtf8(path));
    return;
  }

  const float gain = std::min(1.f, impl_->volume * impl_->game_sfx *
                                     (category == SoundCategory::Laugh ? 1.5f : 1.f));
  if (gain <= 0.001f) {
    LogWrite(std::string("audio: skip ") + label + " (volume silent)");
    return;
  }

  std::vector<std::uint8_t> wav;
  if (!LoadFile(path, wav)) {
    LogWrite(std::string("audio: failed to read ") + label + " " + WideToUtf8(path));
    return;
  }
  if (!ApplyGainToWav(wav, gain)) {
    LogWrite(std::string("audio: skip ") + label + " after gain");
    return;
  }

  PlaySoundW(nullptr, nullptr, 0);
  impl_->play_buf.swap(wav);

  const BOOL ok = PlaySoundW(reinterpret_cast<LPCWSTR>(impl_->play_buf.data()), nullptr,
                             SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
  if (!ok) {
    LogWrite(std::string("audio: PlaySound failed for ") + label + " err=" +
             std::to_string(GetLastError()) + " path=" + WideToUtf8(path));
  } else {
    char buf[96];
    snprintf(buf, sizeof(buf), "audio: play %s #%d gain=%.2f (cfg=%.2f sfx=%.2f)", label, idx,
             gain, impl_->volume, impl_->game_sfx);
    LogWrite(std::string(buf) + " " + WideToUtf8(path));
  }
}

}  // namespace sitcom
