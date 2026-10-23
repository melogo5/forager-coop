// Enemies: the host's AI is authoritative. The host streams every active enemy's position and animation;
// the guest moves its replicas there at the end of each step. The host also keeps the world around the
// guest active (its culling only knows the host's camera).
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::mobs {

// Creates the hooks (enabled by the caller).
bool InstallHooks(bool (*isHost)(), bool (*isGuest)());

// Host, game thread, every local player step while connected.
void HostTick(yyc::CInstance* localPlayer);

// Game thread: applies a mobs message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Game thread: an instance is being destroyed - its id leaves every cull list (mobs.cpp).
void ForgetInstance(yyc::CInstance* self, int id);

// Forgets the last peer's enemies, for the next session.
void Reset();

}  // namespace coop::mobs
