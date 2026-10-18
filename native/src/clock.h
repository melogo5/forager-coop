// Time of day and weather belong to the host's world: once a second the host sends its TimeControl timer
// and light state and WeatherControl's weather timers, and the guest takes them over.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::clock {

// Game thread, every local player step while connected (host only).
void HostTick(yyc::CInstance* localPlayer);

// Game thread: applies a clock message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

}  // namespace coop::clock
