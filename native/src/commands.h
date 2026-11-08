// Chat commands for testing the co-op, typed in the game's own chat (no cheats needed):
//   /puzzlereset <n>  puts puzzle <n> back as it was, at every player (the numbers of the puzzle list, the wiki's
//                     order: see puzzles.h ResetPuzzle)
//   /chestremove      removes the treasure chest nearest to the player, at every player
// The game's chat runs a command's script through script_execute; each command here borrows a script nothing in the
// game ever calls (a third-party library's empty one), registered with the game's own CommandCreate.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::commands {

// Creates the hooks (enabled by the caller).
bool InstallHooks();

// Game thread, every local player step in a world: registers the commands once the game's chat is set up.
void Tick(yyc::CInstance* localPlayer);

// Game thread: applies a command message from another player; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// A world was left: the commands are registered again in the next one.
void Reset();

}  // namespace coop::commands
