// What players do to each other and for fun:
// - bonk: a pickaxe swung at another player (the mouse on them, within reach) flattens them like the meme, for a
//   moment, with stars over the head and a bonk everyone hears. No damage.
// - /dance (chat command, any player, also alone): the character dances - turning side to side, bouncing, notes
//   rising over it, to a chiptune loop of its own (heard while anyone near dances) - until it walks.
// - friendly fire: a blast of our own bomb hurts the other players in its reach (bombs.cpp tells which blasts).
// Poses are only drawn (puppet::SetDrawCallbacks): each player's game draws them itself, so they stay smooth and
// nothing about the body is changed for the game. A bonk and a friendly hit are messages the host routes; dancing
// goes with the player's state (StateFlags).
#pragma once

#include <cstdint>

#include "net.h"
#include "yyc.h"

namespace coop::emotes {

// Creates the hooks (enabled by the caller) and the draw callbacks.
bool InstallHooks(bool (*isHost)());

// Game thread, every local player step (in or out of a session): loads the sprite and sound, registers /dance in
// the chat, stops the dance when the player moves.
void LocalTick(yyc::CInstance* localPlayer);

// What the local player's state tells the others (PlayerStateMsg.flags), and what another player's told us.
uint8_t StateFlags();
void OnRemoteState(net::PeerId player, uint8_t flags);

// Game thread: applies a bonk or a friendly hit; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Forgets the other players (a session ended). The local dance stays.
void Reset();

}  // namespace coop::emotes
