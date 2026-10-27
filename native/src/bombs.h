// Bombs the other players place (PLAYTEST8.md #5): the guest saw neither the host's bomb nor its blast and heard
// nothing. A placed bomb goes to the others as a decoy - it shows, fizzes and ticks but never goes off by itself;
// when the owner's bomb explodes, the decoy turns into a blast that hurts nothing, with its sound. What the blast
// breaks reaches everyone as usual (the owner's world changes).
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

// Forgets the last session.
void Reset();

}  // namespace coop::bombs
