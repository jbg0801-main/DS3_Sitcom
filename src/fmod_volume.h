#pragma once

namespace sitcom {

// Capture in-game SE volume on the FMOD audio thread; worker reads a cached atomic.
bool FmodVolumeInit();
void FmodVolumeShutdown();

// Returns true and writes 0–1 linear gain matching the in-game Sounds slider when known.
bool FmodTryGetSfxVolume(float* out_volume);

}  // namespace sitcom
