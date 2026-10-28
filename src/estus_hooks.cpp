#include "estus_hooks.h"

#include "log.h"

namespace sitcom {

// Empty-flask detection moved to events.cpp via SoulsGym anim name string
// (TAE a000_050114). AOB use-check / drink-gate hooks never saw empty presses
// and are deliberately not installed.

bool EstusHooksInit() {
  LogWrite("estus: AOB hooks disabled (empty flask uses anim name 050114)");
  return true;
}

void EstusHooksShutdown() {}

void EstusHooksNoteAnim(int /*anim*/, const char* /*slot*/) {}

void EstusHooksDebugPulse() {}

int EstusHooksConsumeEmpty() { return 0; }

}  // namespace sitcom
