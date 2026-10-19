// A crafting station (furnace, forge, ...) is used by one player at a time (QUESTIONS.md): while one player has
// its menu open or a craft of its own running there, the other cannot open it. Each side tells the other which
// stations are in use on its side; CreateCraftingMenu does nothing for a station the peer is using.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::stations {

// Creates the hooks (enabled by the caller).
bool InstallHooks();

// Game thread, every local player step while connected (both sides).
void Tick(yyc::CInstance* localPlayer);

// Game thread: applies a station message; false if `message` is not one.
bool Handle(const net::Message& message);

// Forgets which stations are in use (the session is over).
void Reset();

}  // namespace coop::stations
