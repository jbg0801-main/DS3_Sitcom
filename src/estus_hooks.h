#pragma once

namespace sitcom {

// Legacy AOB path is disabled. Empty flask is detected from the player anim
// name string (SoulsGym / TAE a000_050114) in events.cpp.
bool EstusHooksInit();
void EstusHooksShutdown();
void EstusHooksNoteAnim(int anim, const char* slot);
void EstusHooksDebugPulse();
int EstusHooksConsumeEmpty();

}  // namespace sitcom
