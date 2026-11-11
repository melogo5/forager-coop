// Bombs the other players place (PLAYTEST8.md #5): the guest saw neither the host's bomb nor its blast and heard
// nothing. A placed bomb goes to the others as a decoy - it shows, fizzes and ticks but never goes off by itself;
// when the owner's bomb explodes, the decoy turns into a blast that hurts nothing, with its sound. What the blast
// breaks reaches everyone as usual (the owner's world changes), and so does the damage it does to the other players
// (their puppets here: friendly fire).
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::bombs {

// Creates the hooks (enabled by the caller).
bool InstallHooks();

// From the instance_create hook, while connected: `createdId` (object index `object`) was just made.
void OnCreated(yyc::CInstance* self, int createdId, int object);

// Game thread: applies a bomb message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// An objExplosion made here to show another player's blast: its step runs with no damage (as a decoy's blast).
void MakeHarmless(int explosionId);

// True while the step of a blast of our own bomb runs - when it hurts the players in its reach, the other players'
// puppets among them (friendly fire: emotes.cpp sends that damage to them).
bool InOwnBlastStep();

// Forgets the last session.
void Reset();

}  // namespace coop::bombs
