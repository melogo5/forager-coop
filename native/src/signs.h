// Signposts (objSignpost) say their `msg` on E. A sign is a world object: an island bought in the session (the
// desert's "where flowers don't live", the skull island's bells) reached the guests as a new object with no text, and
// a message a player writes on a sign of their own stayed theirs. Each side tells the others a sign's text when it
// changes here; the host also tells all of them now and then (a guest that came later, a sign woken up).
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::signs {

// Game thread, every local player step in the shared room while connected.
void Tick(yyc::CInstance* localPlayer, bool isHost);

// Game thread: applies a sign message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// The session ended or the room changed: forget what the signs said.
void Reset();

}  // namespace coop::signs
