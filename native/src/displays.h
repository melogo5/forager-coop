// Display cases (objDisplayCase) show the item a player put in (`socket`, noone when empty): the item went in on
// one side only, so each player could fill the same case with something of their own. Each side watches its cases
// and tells the others when one of them changed here; the case at the same spot there takes the same item.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::displays {

// Game thread, every local player step while connected (both sides).
void Tick(yyc::CInstance* localPlayer);

// Game thread: applies a display case message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// The session ended or the world room went away: forget what the cases held.
void Reset();

}  // namespace coop::displays
