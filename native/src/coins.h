// Coins are shared (QUESTIONS.md) and kept by the host, in its player's `coins`. The guest notices its own coin
// changes (earned, spent) and sends the difference to the host; the host sends the total twice a second.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::coins {

// Game thread, every local player step while connected.
void HostTick(yyc::CInstance* localPlayer);
void GuestTick(yyc::CInstance* localPlayer);

// Game thread: applies a coins message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Forgets the session's coin bookkeeping.
void Reset();

}  // namespace coop::coins
