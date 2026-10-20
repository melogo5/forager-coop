// Steam, through the game's own steam_api.dll (the runner has already initialised it): lobbies and the overlay's
// invite dialog, for joining a friend's game by Steam invite (docs/coop/STEAM.md).
#pragma once

#include "yyc.h"

namespace coop::steam {

// Game thread, every main menu step. Testing aid for now: COOP_DEBUG_STEAM=1 checks what the plan relies on -
// the Steam interfaces answer, the game runs Steam callbacks every frame, a friends-only lobby can be created and
// the overlay's invite dialog opens for it - and logs each step.
void Tick(yyc::CInstance* self);

}  // namespace coop::steam
