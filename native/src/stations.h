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
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// The station shows the other player's craft (it must not make anything itself: the crafter gets the output).
bool IsMirror(yyc::CInstance* station);

// The local player says (speech bubble) that the other player is using what it tried to open - at most every
// few seconds, so clicking again and again does not stack bubbles.
void SayBusy(yyc::CInstance* player);
// A speech bubble over `player` (the game's Dialogue).
void Say(yyc::CInstance* player, const char* text);

// Forgets which stations are in use (the session is over).
// Host: a guest that has just joined hears which stations are in use (by the host or another guest).
void OnPeerJoined(net::PeerId peer);
// Host: guest `peer` left; stations it used are free again, for everyone.
void OnPeerLeft(net::PeerId peer);

void Reset();

}  // namespace coop::stations
