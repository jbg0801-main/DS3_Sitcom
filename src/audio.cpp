#include "audio.h"

#include "log.h"
#include "paths.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

bool ApplyGainToPcm(std::vector<std::uint8_t>& pcm, float gain, std::uint16_t bits) {
  if (gain >= 0.999f) {
    return gain > 0.001f;
  }
  if (gain <= 0.001f) {
    return false;
  }
  if (bits == 16) {
    auto* samples = reinterpret_cast<std::int16_t*>(pcm.data());
    const std::size_t n = pcm.size() / 2;
    for (std::size_t i = 0; i < n; ++i) {
      const float v = static_cast<float>(samples[i]) * gain;
      const int clamped = static_cast<int>(std::lround(v));
      samples[i] = static_cast<std::int16_t>(std::max(-32768, std::min(32767, clamped)));
    }
  } else if (bits == 8) {
    for (std::size_t i = 0; i < pcm.size(); ++i) {
      const float centered = (static_cast<float>(pcm[i]) - 128.f) * gain + 128.f;
      const int clamped = static_cast<int>(std::lround(centered));
      pcm[i] = static_cast<std::uint8_t>(std::max(0, std::min(255, clamped)));
    }
  }
  return true;
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

  if (data_off == 0 || data_size == 0) {
    return true;
  }
  std::vector<std::uint8_t> pcm(wav.begin() + static_cast<std::ptrdiff_t>(data_off),
                                wav.begin() + static_cast<std::ptrdiff_t>(data_off + data_size));
  if (!ApplyGainToPcm(pcm, gain, bits)) {
    return false;
  }
  std::memcpy(wav.data() + data_off, pcm.data(), pcm.size());
  return true;
}

bool ParseWavPcm(const std::vector<std::uint8_t>& wav, WAVEFORMATEX& fmt_out,
                 std::vector<std::uint8_t>& pcm_out) {
  if (wav.size() < 44 || memcmp(wav.data(), "RIFF", 4) != 0 || memcmp(wav.data() + 8, "WAVE", 4) != 0) {
    return false;
  }

  std::uint16_t audio_format = 1;
  std::uint16_t channels = 1;
  std::uint32_t sample_rate = 44100;
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
      channels = *reinterpret_cast<const std::uint16_t*>(wav.data() + payload + 2);
      sample_rate = *reinterpret_cast<const std::uint32_t*>(wav.data() + payload + 4);
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

  if (data_off == 0 || data_size == 0 || audio_format != 1 || channels == 0 || bits == 0) {
    return false;
  }

  memset(&fmt_out, 0, sizeof(fmt_out));
  fmt_out.wFormatTag = WAVE_FORMAT_PCM;
  fmt_out.nChannels = channels;
  fmt_out.nSamplesPerSec = sample_rate;
  fmt_out.wBitsPerSample = bits;
  fmt_out.nBlockAlign = static_cast<WORD>((channels * bits) / 8);
  fmt_out.nAvgBytesPerSec = fmt_out.nSamplesPerSec * fmt_out.nBlockAlign;
  fmt_out.cbSize = 0;

  pcm_out.assign(wav.data() + data_off, wav.data() + data_off + data_size);
  return true;
}

}  // namespace

struct WavePlayback {
  HWAVEOUT hwo = nullptr;
  WAVEHDR hdr{};
  std::vector<std::uint8_t> pcm;
};

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
  WavePlayback* playback = nullptr;
  WavePlayback* laugh_playback = nullptr;
  std::vector<std::uint8_t> play_buf;
};

void Audio::FreeLaughPlayback(Impl* impl) {
  if (!impl || !impl->laugh_playback) {
    return;
  }
  WavePlayback* pb = impl->laugh_playback;
  impl->laugh_playback = nullptr;
  if (pb->hwo) {
    waveOutReset(pb->hwo);
    if (pb->hdr.lpData) {
      waveOutUnprepareHeader(pb->hwo, &pb->hdr, sizeof(WAVEHDR));
    }
    waveOutClose(pb->hwo);
  }
  delete pb;
}

void Audio::FreePlayback(Impl* impl) {
  if (!impl || !impl->playback) {
    return;
  }
  WavePlayback* pb = impl->playback;
  impl->playback = nullptr;
  if (pb->hwo) {
    waveOutReset(pb->hwo);
    if (pb->hdr.lpData) {
      waveOutUnprepareHeader(pb->hwo, &pb->hdr, sizeof(WAVEHDR));
    }
    waveOutClose(pb->hwo);
  }
  delete pb;
}

void Audio::OnWaveOutDone(void* hwo_ptr) {
  const HWAVEOUT hwo = static_cast<HWAVEOUT>(hwo_ptr);
  if (!impl_) {
    return;
  }
  if (impl_->laugh_playback && impl_->laugh_playback->hwo == hwo) {
    WavePlayback* pb = impl_->laugh_playback;
    impl_->laugh_playback = nullptr;
    if (pb->hdr.lpData) {
      waveOutUnprepareHeader(hwo, &pb->hdr, sizeof(WAVEHDR));
    }
    waveOutClose(hwo);
    delete pb;
    return;
  }
  if (impl_->playback && impl_->playback->hwo == hwo) {
    WavePlayback* pb = impl_->playback;
    impl_->playback = nullptr;
    if (pb->hdr.lpData) {
      waveOutUnprepareHeader(hwo, &pb->hdr, sizeof(WAVEHDR));
    }
    waveOutClose(hwo);
    delete pb;
  }
}

namespace {

void CALLBACK SitcomWaveOutProc(HWAVEOUT hwo, UINT msg, DWORD_PTR instance, DWORD_PTR /*param1*/,
                                DWORD_PTR /*param2*/) {
  if (msg != WOM_DONE) {
    return;
  }
  auto* audio = reinterpret_cast<Audio*>(instance);
  if (audio) {
    audio->OnWaveOutDone(hwo);
  }
}

}  // namespace

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
  FreePlayback(impl_);
  FreeLaughPlayback(impl_);
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

  WAVEFORMATEX fmt{};
  std::vector<std::uint8_t> pcm;
  if (!ParseWavPcm(wav, fmt, pcm)) {
    LogWrite(std::string("audio: unsupported WAV for ") + label + " " + WideToUtf8(path));
    return;
  }
  if (!ApplyGainToPcm(pcm, gain, fmt.wBitsPerSample)) {
    LogWrite(std::string("audio: skip ") + label + " after gain");
    return;
  }

  WavePlayback** slot = &impl_->playback;
  if (category == SoundCategory::Laugh) {
    FreeLaughPlayback(impl_);
    slot = &impl_->laugh_playback;
  } else {
    FreePlayback(impl_);
  }

  auto* pb = new WavePlayback();
  pb->pcm = std::move(pcm);
  *slot = pb;

  const MMRESULT open_res =
      waveOutOpen(&pb->hwo, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(SitcomWaveOutProc),
                  reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
  if (open_res != MMSYSERR_NOERROR || !pb->hwo) {
    LogWrite(std::string("audio: waveOutOpen failed for ") + label + " err=" +
             std::to_string(open_res));
    *slot = nullptr;
    delete pb;
    return;
  }

  pb->hdr.lpData = reinterpret_cast<LPSTR>(pb->pcm.data());
  pb->hdr.dwBufferLength = static_cast<DWORD>(pb->pcm.size());
  pb->hdr.dwFlags = 0;

  MMRESULT prep_res = waveOutPrepareHeader(pb->hwo, &pb->hdr, sizeof(WAVEHDR));
  if (prep_res != MMSYSERR_NOERROR) {
    LogWrite(std::string("audio: waveOutPrepareHeader failed for ") + label + " err=" +
             std::to_string(prep_res));
    *slot = nullptr;
    delete pb;
    return;
  }

  const MMRESULT write_res = waveOutWrite(pb->hwo, &pb->hdr, sizeof(WAVEHDR));
  if (write_res != MMSYSERR_NOERROR) {
    LogWrite(std::string("audio: waveOutWrite failed for ") + label + " err=" +
             std::to_string(write_res));
    *slot = nullptr;
    delete pb;
    return;
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "audio: waveOut play %s #%d gain=%.2f (cfg=%.2f sfx=%.2f)", label, idx,
           gain, impl_->volume, impl_->game_sfx);
  LogWrite(std::string(buf) + " " + WideToUtf8(path));
}

void Audio::PlayTitleSmokeTest(SoundCategory category) {
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
    default:
      Play(category);
      return;
  }
  const int idx = PickIndex(impl_->rng, static_cast<int>(list->size()), *last);
  if (idx < 0) {
    return;
  }
  *last = idx;
  const std::wstring& path = (*list)[idx];
  const float gain = std::min(1.f, impl_->volume * impl_->game_sfx * 1.5f);
  std::vector<std::uint8_t> wav;
  if (!LoadFile(path, wav) || !FileExists(path)) {
    return;
  }
  const float play_gain = gain;
  if (play_gain <= 0.001f) {
    return;
  }
  PlaySoundW(path.c_str(), nullptr, SND_ASYNC | SND_FILENAME | SND_NODEFAULT);
  LogWrite(std::string("audio: PlaySound title smoke-test ") + label + " gain=" +
           std::to_string(play_gain) + " " + WideToUtf8(path));
}

}  // namespace sitcom
