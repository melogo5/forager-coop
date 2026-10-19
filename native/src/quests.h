// NPC quests are shared (QUESTIONS.md): either player brings the items from its own inventory (and gets the
// reward there); the quest moves on for both. Each side looks at its NPCs once a second (they are unique: one
// per kind) and sends a changed quest/state/hasSpoken; the peer sets its NPC the same way, through the game's
// own NPCSetQuest/NPCSetState so the next requirement is set up as the game does it.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::quests {

// Game thread, every local player step while connected (both sides).
void Tick(yyc::CInstance* localPlayer);

// Game thread: applies a quest message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Forgets what was last seen (the session is over).
void Reset();

}  // namespace coop::quests
