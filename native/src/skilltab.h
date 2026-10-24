// The skill tree in the world: in a coop session the game's skill grid (rmSkillGrid) is not a room of its own -
// going there would unload the world (it reloads from the save on the way back), stop the player's furnaces and,
// for the host, freeze the world for everyone (PLAYTEST2). Its objects (objSkillGridControl and the objSkill it
// makes) run in the world room instead, drawn over the world in the GUI layer; the world goes on behind it.
#pragma once

#include "yyc.h"

namespace coop::skilltab {

// Creates the hooks (enabled by the caller). `inSession`: whether a coop session is on.
bool InstallHooks(bool (*inSession)());

// From the room_goto hook: true if the call is taken care of here (the tree opens or closes in the world room).
bool InterceptRoomGoto(yyc::CInstance* self, int room);

// The tree is open: the local player stands still (puppet::HoldLocalPlayer), the world does not.
bool IsOpen();

// Another player's skills came while the tree is open here: each skill shows its state again (it is set once, on
// opening; the game itself only changes the skill bought here). `self`: an instance to call builtins as.
void RefreshStates(yyc::CInstance* self);

// The session ended or the world room went away: forget the tree.
void Reset();

// Testing aid: COOP_DEBUG_SKILLTREE=<seconds> goes to the skill tree that late in the world (as the game does,
// room_goto), and closes it 10 s later.
void DebugTick(yyc::CInstance* localPlayer);

}  // namespace coop::skilltab
