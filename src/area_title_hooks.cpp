#include "area_title_hooks.h"

#include "log.h"

namespace sitcom {

// F23 index-8 / clip-player / FrpgMenuDlgTE_MapName heap scan produced zero
// title hits across Cemetery↔Firelink playtests (F23 any:0). Walk-in wipe is
// driven by TGA SprjFade Title RemainTiem in events.cpp; bonfire warps still
// use the world-block edge.

bool AreaTitleHooksInit() {
  LogWrite("area_title: F23/clip/map-name hooks disabled (wipe via Title fade / world block)");
  return true;
}

void AreaTitleHooksShutdown() {}

void AreaTitleHooksPoll() {}

int AreaTitleHooksConsume() { return 0; }

}  // namespace sitcom
