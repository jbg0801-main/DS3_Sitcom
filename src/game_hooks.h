#pragma once

#include <cstdint>

namespace sitcom {

// MOD_FINDINGS.md: ApplyDamageToHp, Chr_Kill hooks.
// Boss cheer/defeat and ladder fall are polled (gauge slots / ladder act byte).
bool GameHooksInit();
void GameHooksShutdown();

// Consume counts since last poll (worker thread).
int GameHooksConsumeEmptyEstus();
int GameHooksConsumeAreaTitle();
int GameHooksConsumeHit();
int GameHooksConsumeDeath();
int GameHooksConsumeBossCheer();
int GameHooksConsumeBossApplause();
int GameHooksConsumeLadderFall();

// Place id from last QueueMenu(8) note, or -1.
std::int32_t GameHooksLastPlaceNameId();

// Live last-shown place name id (g_MapNameState+0x1A3C), or -1 if unread.
std::int32_t GameHooksReadShownPlaceNameId();

}  // namespace sitcom
