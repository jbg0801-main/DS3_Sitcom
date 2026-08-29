#pragma once

namespace sitcom {

// Capture in-game SE volume on the FMOD audio thread; worker reads a cached atomic.
bool FmodVolumeInit();
void FmodVolumeShutdown();
void FmodSetProbeTransitions(bool enabled);

// Returns true and writes 0–1 linear gain matching the in-game Sounds slider when known.
bool FmodTryGetSfxVolume(float* out_volume);

// Area title / map transition SFX detected on the FMOD audio thread (FMOD_Event_Start hook).
bool FmodConsumeAreaTransition(char* out_name, int out_name_cap);

}  // namespace sitcom
