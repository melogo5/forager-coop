// Each player has its own skills (docs/coop/PLAYTEST.md #9); level and XP stay shared. The world save holds
// only the host's skills, so the guest's are kept by the host next to its character
// (saveDataN/coop_guests/<name>.skills) and put back after the guest loads the host's world: its own learned
// skills instead of the host's, and its own skill points. Every shared level-up earns the guest a point too.
#pragma once

#include <string>

#include "net.h"
#include "yyc.h"

namespace coop::skills {

// Host, game thread: the guest has introduced itself as `name`; sends what is stored for it.
void HostOnGuest(const std::string& name);

// Game thread, every local player step while connected (both sides; the work is the guest's).
void Tick(yyc::CInstance* localPlayer);

// Game thread: handles a skills message (host: stores it; guest: applies it); false if not one.
bool Handle(const net::Message& message);

// Forgets the session (the guest re-applies on joining again).
void Reset();

}  // namespace coop::skills
