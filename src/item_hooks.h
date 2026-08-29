#pragma once

#include <cstdint>

namespace sitcom {

// One-shot / permanent hooks that set pollable flags for the worker thread.
bool ItemHooksInit();
void ItemHooksShutdown();

// Number of ItemGet calls since last Consume (may be >1 for multi-item lots).
// Estus flask re-grants (ids 200–215, e.g. bonfire rest) are excluded.
int ItemHooksConsumeAcquired();

// Estus ItemGets swallowed by the filter (for optional debug logging).
int ItemHooksConsumeSuppressed();

int ItemHooksLastItemId();
int ItemHooksLastCategory();

}  // namespace sitcom
