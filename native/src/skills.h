// Each player has its own skills (docs/coop/PLAYTEST.md #9); level and XP stay shared. The world save holds
// only the host's skills, so the guest's are kept by the host next to its character
// (saveDataN/coop_guests/<name>.skills) and put back after the guest loads the host's world: its own learned
// skills instead of the host's, and its own skill points. Every shared level-up earns the guest a point too.
#pragma once

#include <string>

#include "net.h"
#include "yyc.h"

namespace coop::skills {

// Host, game thread: guest `peer` has introduced itself as `name`; sends it what is stored for it.
void HostOnGuest(const std::string& name, net::PeerId peer);

// Game thread, every local player step while connected (both sides; the work is the guest's).
void Tick(yyc::CInstance* localPlayer);

// Game thread: handles a skills message (host: stores it; guest: applies it); false if not one.
bool Handle(const net::Message& message);

// Host, game thread: while the host acts as guest `player` (puppet::BeginActing: its hits, builds, what they
// destroy), global.skillUnlocked holds that guest's skills - drops and bonuses follow the guest's skills, not the
// host's (playtest 4, bug 7). `puppet`: any instance to call builtins as. SwapOut puts the host's back.
void HostSwapIn(yyc::CInstance* puppet, net::PeerId player);
void HostSwapOut();

// Forgets the session (the guest re-applies on joining again).
void Reset();

}  // namespace coop::skills
