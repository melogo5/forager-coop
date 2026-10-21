// Interiors (wizard tower, druid tree, museum: rooms 22-24) - research for building them inside the world room
// (docs/coop/INTERIORS.md). For now a testing aid only: COOP_DEBUG_DUMP_ROOMS=1 writes what an interior is made of
// (instances, layers with their tiles, the view) to the log, once per room, 2 s after entering it.
#pragma once

#include "yyc.h"

namespace coop::interiors {

// Game thread, every local player step (in any room).
void DebugTick(yyc::CInstance* localPlayer);

}  // namespace coop::interiors
