// Fishing log (PLAYTEST2 #6: a guest's bobber stays at "caught" and cannot be reeled in). Changes nothing: the
// bobber's events are logged as they run, with who called them, and its state when it changes.
#pragma once

#include "yyc.h"

namespace coop::fishing {

// Creates the hooks (enabled by the caller). `inSession`: whether a coop session is on (nothing is logged outside).
bool InstallHooks(bool (*inSession)());

// From the local player's step.
void Tick(yyc::CInstance* localPlayer);

void Reset();

}  // namespace coop::fishing
