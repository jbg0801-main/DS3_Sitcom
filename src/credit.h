#pragma once

namespace sitcom {

// Draws "Sitcom mod by jbg0801 2026" over the game window while on the title screen.
void CreditEnsureHook();
void CreditUpdate(bool on_title_screen);
void CreditShutdown();

}  // namespace sitcom
