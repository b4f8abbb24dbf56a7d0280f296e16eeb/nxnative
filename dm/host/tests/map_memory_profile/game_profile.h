// Synthetic MapMemory test profile; enough stack space and pool budget to hit
// the production 4096-record metadata limit before address or pool exhaustion.
#pragma once
#include "../address_space_profile/game_profile.h"
#undef DM_GAME_MEM_STACK
#define DM_GAME_MEM_STACK UINT64_C(0x02000000)
#undef DM_GAME_APP_POOL
#define DM_GAME_APP_POOL UINT64_C(0x02000000)
