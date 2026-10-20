// Costumes (PLAYTEST2.md #8). What a player wears is objPlayer.cosmetic (an id per slot); the world save and the
// options file keep it, so a guest that loads the host's world wore the host's costume and its options took it
// over. Each player's own costume is kept in coop_look.txt next to the saves (from its own worlds too), a guest puts
// it back on in the host's world, and every player's costume goes with its state to the others' puppets.
#pragma once

#include <cstdint>

#include "yyc.h"

namespace coop::look {

constexpr int kMaxSlots = 8;

#pragma pack(push, 1)
struct Look {
    uint8_t count = 0;  // 0: not known
    int16_t items[kMaxSlots] = {};
};
#pragma pack(pop)

bool operator==(const Look& a, const Look& b);
inline bool operator!=(const Look& a, const Look& b) { return !(a == b); }

// What `player` wears now.
Look Read(yyc::CInstance* player);
// Dresses `player` (a puppet, or the local player).
void Apply(yyc::CInstance* player, const Look& look);

// Game thread, every local player step. `guest`: in the host's world as a guest; `inWorld`: in the world room.
void Tick(yyc::CInstance* localPlayer, bool guest, bool inWorld);

// The session is over: a guest puts its own costume on again when it next joins.
void Reset();

}  // namespace coop::look
