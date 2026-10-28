#pragma once

namespace sitcom {

// Legacy F23/clip/map-name hooks are disabled. Scene wipe uses SprjFade Title
// RemainTiem (TGA) and world-block changes in events.cpp.
bool AreaTitleHooksInit();
void AreaTitleHooksShutdown();
void AreaTitleHooksPoll();
int AreaTitleHooksConsume();

}  // namespace sitcom
